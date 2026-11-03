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
#include <math.h>

/* The small helpers here are forced inline: in the game code's huge
   functions the compiler otherwise keeps many of them as real calls, one
   per guest instruction. (Only small ones, and none that take the address
   of a local: those would each get their own stack space in the huge
   functions, which then need kilobytes of stack per call.) */
#if defined(__clang__) || defined(__GNUC__)
#define FM_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define FM_INLINE static __forceinline
#else
#define FM_INLINE static inline
#endif

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

/* Direct calls between chunks (tools/directcalls.c rewrites the generated
   code to use these): allowed unless the port hooks the target (a bit in
   g_nfls2_direct_off, by word address) or has them all off (a debug trace
   on); a depth limit keeps the host stack small. */
extern unsigned char g_nfls2_direct_off[0x02000000u >> 5];
extern int g_nfls2_direct_on;
extern unsigned g_nfls2_direct_depth;
#define NFLS2_DIRECT_OK(t) \
    (g_nfls2_direct_on && !(g_nfls2_direct_off[((t) & 0x01FFFFFFu) >> 5] & (1u << ((((t) & 0x01FFFFFFu) >> 2) & 7u))))
#define NFLS2_DIRECT_MAX_DEPTH 48
extern unsigned long long g_nfls2_direct_calls;
FM_INLINE int nfls2_direct_enter(void) {
    if (g_nfls2_direct_depth >= NFLS2_DIRECT_MAX_DEPTH) return 0;
    g_nfls2_direct_depth++;
    g_nfls2_direct_calls++;
    return 1;
}
FM_INLINE void nfls2_direct_leave(void) { g_nfls2_direct_depth--; }

#define FM_RAM(cpu, addr, n, off) (((off) = (addr) - 0x80000000u) <= (cpu)->ram_size - (n))

FM_INLINE u8 fm_read8(CPUState* cpu, u32 addr) {
    u32 off;
    if (FM_RAM(cpu, addr, 1u, off))
        return cpu->ram[off];
    return (mem_read8)(cpu, addr);
}

FM_INLINE u16 fm_read16(CPUState* cpu, u32 addr) {
    u32 off;
    if (FM_RAM(cpu, addr, 2u, off)) {
        u16 v;
        memcpy(&v, cpu->ram + off, 2);
        return FM_BSWAP16(v);
    }
    return (mem_read16)(cpu, addr);
}

FM_INLINE u32 fm_read32(CPUState* cpu, u32 addr) {
    u32 off;
    if (FM_RAM(cpu, addr, 4u, off)) {
        u32 v;
        memcpy(&v, cpu->ram + off, 4);
        return FM_BSWAP32(v);
    }
    return (mem_read32)(cpu, addr);
}

FM_INLINE u64 fm_read64(CPUState* cpu, u32 addr) {
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
FM_INLINE void fm_write8(CPUState* cpu, u32 addr, u8 value) {
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

FM_INLINE void fm_write16(CPUState* cpu, u32 addr, u16 value) {
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

FM_INLINE void fm_write32(CPUState* cpu, u32 addr, u32 value) {
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

FM_INLINE void fm_write64(CPUState* cpu, u32 addr, u64 value) {
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
FM_INLINE bool fm_f32_load_ok(u32 bits) {
    return ((bits >> 23) & 0xFFu) != 0xFFu || (bits & 0x007FFFFFu) == 0;
}

/* Stores: the full version flushes denormal results to zero. */
FM_INLINE bool fm_f32_store_ok(u32 bits) {
    return ((bits >> 23) & 0xFFu) != 0 || (bits & 0x007FFFFFu) == 0;
}

/* Quantised stores: a value whose product with 2^scale (|scale| < 32) is
   exact, neither overflowing nor coming near a denormal. */
FM_INLINE bool ff_quant_ok(f64 v) { return v == 0.0 || (fabs(v) < 1e30 && fabs(v) > 1e-280); }
FM_INLINE s32 ff_quant(f64 scaled, s32 lo, s32 hi) {
    f64 t = trunc(scaled);
    if (t <= (f64)lo) return lo;
    if (t >= (f64)hi) return hi;
    return (s32)t;
}

FM_INLINE bool fm_psq_ok(CPUState* cpu, bool indexed) {
    return (cpu->hid2 & PPC_HID2_PSE) && (indexed || (cpu->hid2 & PPC_HID2_LSQE));
}

#undef ppc_psq_load_inline
#undef ppc_psq_store_inline
FM_INLINE bool fm_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
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
    /* Quantised loads (u8 / u16 / s8 / s16 scaled by a power of two, the
       way models store positions and normals): the integer times 2^-scale
       is exact in a double, so rounding it to single gives what the core's
       ldexp() does. */
    u32 q = cpu->gqr[gqr & 7u];
    u32 type = (q >> 16) & 7u;
    if (type >= 4 && fm_psq_ok(cpu, indexed)) {
        u32 size = (type & 1u) ? 2u : 1u;
        if (FM_RAM(cpu, ea, w ? size : size * 2u, off)) {
            s32 sc = (s32)((q >> 24) & 0x3Fu);
            if (sc & 0x20) sc -= 0x40;                     /* (6-bit signed) */
            u64 pb = (u64)(1023 - sc) << 52;               /* 2^-scale */
            f64 mul;
            memcpy(&mul, &pb, 8);
            const u8* p = cpu->ram + off;
            s32 v0, v1 = 0;
            if (size == 1) {
                v0 = type == 6 ? (s32)(s8)p[0] : (s32)p[0];
                if (!w) v1 = type == 6 ? (s32)(s8)p[1] : (s32)p[1];
            } else {
                u32 a0 = ((u32)p[0] << 8) | p[1];
                v0 = type == 7 ? (s32)(s16)a0 : (s32)a0;
                if (!w) {
                    u32 a1 = ((u32)p[2] << 8) | p[3];
                    v1 = type == 7 ? (s32)(s16)a1 : (s32)a1;
                }
            }
            cpu->fpr[frD] = (f64)(f32)((f64)v0 * mul);
            cpu->ps1[frD] = w ? 1.0 : (f64)(f32)((f64)v1 * mul);
            return true;
        }
    }
    return ppc_psq_load(cpu, frD, ea, w, gqr, indexed, cia);
}

FM_INLINE bool fm_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
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
    /* Quantised stores (u8 / u16 / s8 / s16, times 2^scale, cut toward zero
       and clamped; vertex data into the graphics pipe, mostly): as the core
       works them out when the value times 2^scale is exact in a double. */
    u32 q = cpu->gqr[gqr & 7u];
    u32 type = q & 7u;
    if (type >= 4 && fm_psq_ok(cpu, indexed)) {
        f64 v0 = cpu->fpr[frS], v1 = w ? 0.0 : cpu->ps1[frS];
        if (ff_quant_ok(v0) && ff_quant_ok(v1)) {
            s32 sc = (s32)((q >> 8) & 0x3Fu);
            if (sc & 0x20) sc -= 0x40;                     /* (6-bit signed) */
            u64 pb = (u64)(1023 + sc) << 52;               /* 2^scale */
            f64 mul;
            memcpy(&mul, &pb, 8);
            s32 lo = type == 4 || type == 5 ? 0 : type == 6 ? -128 : -32768;
            s32 hi = type == 4 ? 255 : type == 5 ? 65535 : type == 6 ? 127 : 32767;
            s32 i0 = ff_quant(v0 * mul, lo, hi), i1 = ff_quant(v1 * mul, lo, hi);
            if (type & 1u) {
                fm_write16(cpu, ea, (u16)i0);
                if (!w) fm_write16(cpu, ea + 2u, (u16)i1);
            } else {
                fm_write8(cpu, ea, (u8)i0);
                if (!w) fm_write8(cpu, ea + 1u, (u8)i1);
            }
            return true;
        }
    }
    return ppc_psq_store(cpu, frS, ea, w, gqr, indexed, cia);
}

#define ppc_psq_load_inline(cpu, frD, ea, w, gqr, indexed, cia) fm_psq_load((cpu), (frD), (ea), (w), (gqr), (indexed), (cia))
#define ppc_psq_store_inline(cpu, frS, ea, w, gqr, indexed, cia) fm_psq_store((cpu), (frS), (ea), (w), (gqr), (indexed), (cia))

/* Floating point: every float instruction called into the CPU core in
 * another file, which also keeps the FPSCR's status bits (inexact, fraction
 * rounded, result class) exact. The game never reads those (only its thread
 * switches save and restore the FPSCR, and it runs with round-to-nearest and
 * no float exceptions enabled), so with that setup the result alone is
 * worked out here, inline, the same value the core gives: same double
 * arithmetic, same rounding to single, Gekko's 25-bit multiplier. Anything
 * unusual (NaN, infinity, division by zero, denormal multiplier, another
 * rounding mode) goes to the full version.
 */
#ifndef NFLS2_NO_FASTFP
#include <math.h>

#define FF_PLAIN(cpu) (((cpu)->fpscr & 0xFFu) == 0)   /* round to nearest, no exception enables, NI off */

FM_INLINE u64 ff_bits(f64 v) { u64 b; memcpy(&b, &v, 8); return b; }
FM_INLINE f64 ff_value(u64 b) { f64 v; memcpy(&v, &b, 8); return v; }
FM_INLINE bool ff_finite(f64 v) { return (ff_bits(v) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull; }

/* Gekko rounds a single multiply's second operand to 25 bits (normal
   numbers; a denormal one isn't handled here). */
FM_INLINE bool ff_25_ok(f64 c) {
    u64 b = ff_bits(c);
    return !((b & 0x7FF0000000000000ull) == 0 && (b & 0x000FFFFFFFFFFFFFull) != 0);
}
FM_INLINE f64 ff_25v(f64 c) {
    u64 b = ff_bits(c);
    return ff_value((b & 0xFFFFFFFFF8000000ull) + (b & 0x0000000008000000ull));
}

FM_INLINE bool ff_single(CPUState* cpu, u8 d, f64 r) {
    f32 f = (f32)r;
    f64 fd = (f64)f;
    if (!ff_finite(fd)) return false;   /* (NaN, or overflow: the full version) */
    cpu->fpr[d] = fd;
    cpu->ps1[d] = fd;
    return true;
}

FM_INLINE void fm_fadds(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (FF_PLAIN(cpu) && ff_single(cpu, d, cpu->fpr[a] + cpu->fpr[b])) return;
    (ppc_fadds)(cpu, d, a, b);
}
FM_INLINE void fm_fsubs(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (FF_PLAIN(cpu) && ff_single(cpu, d, cpu->fpr[a] - cpu->fpr[b])) return;
    (ppc_fsubs)(cpu, d, a, b);
}
FM_INLINE void fm_fmuls(CPUState* cpu, u8 d, u8 a, u8 c) {
    if (FF_PLAIN(cpu) && ff_25_ok(cpu->fpr[c]) && ff_single(cpu, d, cpu->fpr[a] * ff_25v(cpu->fpr[c]))) return;
    (ppc_fmuls)(cpu, d, a, c);
}
FM_INLINE void fm_fdivs(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (FF_PLAIN(cpu) && cpu->fpr[b] != 0.0 && ff_single(cpu, d, cpu->fpr[a] / cpu->fpr[b])) return;
    (ppc_fdivs)(cpu, d, a, b);
}

FM_INLINE bool ff_double(CPUState* cpu, u8 d, f64 r) {
    if (!ff_finite(r)) return false;
    cpu->fpr[d] = r;
    return true;
}
FM_INLINE void fm_fadd(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (FF_PLAIN(cpu) && ff_double(cpu, d, cpu->fpr[a] + cpu->fpr[b])) return;
    (ppc_fadd)(cpu, d, a, b);
}
FM_INLINE void fm_fsub(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (FF_PLAIN(cpu) && ff_double(cpu, d, cpu->fpr[a] - cpu->fpr[b])) return;
    (ppc_fsub)(cpu, d, a, b);
}
FM_INLINE void fm_fmul(CPUState* cpu, u8 d, u8 a, u8 c) {
    if (FF_PLAIN(cpu) && ff_double(cpu, d, cpu->fpr[a] * cpu->fpr[c])) return;
    (ppc_fmul)(cpu, d, a, c);
}
FM_INLINE void fm_fdiv(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (FF_PLAIN(cpu) && cpu->fpr[b] != 0.0 && ff_double(cpu, d, cpu->fpr[a] / cpu->fpr[b])) return;
    (ppc_fdiv)(cpu, d, a, b);
}
FM_INLINE void fm_frsp(CPUState* cpu, u8 d, u8 b) {
    if (FF_PLAIN(cpu) && ff_finite(cpu->fpr[b]) && ff_single(cpu, d, cpu->fpr[b])) return;
    (ppc_frsp)(cpu, d, b);
}

/* Compares: the condition field and FPCC exactly as the core sets them (a
   NaN, which can raise an exception, goes to it). */
FM_INLINE void fm_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered) {
    if (a != a || b != b) { (ppc_fcmp)(cpu, crfd, a, b, ordered); return; }
    u32 compare = a < b ? 8u : a > b ? 4u : 2u;
    cpu->fpscr = (cpu->fpscr & ~(0xFu << 12)) | (compare << 12);
    u32 shift = 4u * (7u - crfd);
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (compare << shift);
}

/* Single multiply-add: with a a single-precision value and c rounded to 25
   bits, a*c is exact in a double, so a*c + b rounds once, exactly as fma()
   does (fma() itself is slow without the FMA instruction, which the build
   doesn't assume). A result exactly halfway between two singles (which the
   core corrects with a second look) and anything unusual go to the core;
   double multiply-adds too. */
static inline bool fm_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative,
                          f64* output) {
    if (single && FF_PLAIN(cpu) && ff_finite(a) && (f64)(f32)a == a && fabs(c) < 1e38 &&
        (c == 0.0 || fabs(c) > 1e-200) && ff_finite(b)) {   /* (a*c can't overflow or underflow) */
        if (ff_25_ok(c)) {
            f64 c25 = ff_25v(c);
            f64 r = a * c25 + (subtract ? -b : b);
            u64 rb = ff_bits(r);
            if ((rb & 0x000000001FFFFFFFull) != 0x0000000010000000ull) {   /* (not a tie for single) */
                f64 s = (f64)(f32)r;
                if (ff_finite(s)) {
                    *output = negative ? -s : s;
                    return true;
                }
            }
        }
    }
    return (ppc_fma)(cpu, a, c, b, single, subtract, negative, output);
}

/* Paired singles: each half as the core works it out (the same double
   arithmetic and 25-bit multiplier, rounded to single), unless a NaN turns
   up (its payload rules are the core's). */
FM_INLINE bool ff_nan(f64 v) { return v != v; }
FM_INLINE bool ff_pair(CPUState* cpu, u8 d, f64 p0, f64 p1) {
    if (ff_nan(p0) || ff_nan(p1)) return false;
    cpu->fpr[d] = (f64)(f32)p0;
    cpu->ps1[d] = (f64)(f32)p1;
    return true;
}
FM_INLINE void fm_ps_add(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (ff_pair(cpu, d, cpu->fpr[a] + cpu->fpr[b], cpu->ps1[a] + cpu->ps1[b])) return;
    (ppc_ps_add_op)(cpu, d, a, b);
}
FM_INLINE void fm_ps_sub(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (ff_pair(cpu, d, cpu->fpr[a] - cpu->fpr[b], cpu->ps1[a] - cpu->ps1[b])) return;
    (ppc_ps_sub_op)(cpu, d, a, b);
}
FM_INLINE void fm_ps_mul(CPUState* cpu, u8 d, u8 a, u8 c) {
    if (ff_25_ok(cpu->fpr[c]) && ff_25_ok(cpu->ps1[c]) && !ff_nan(cpu->fpr[c]) && !ff_nan(cpu->ps1[c]) &&
        ff_pair(cpu, d, cpu->fpr[a] * ff_25v(cpu->fpr[c]), cpu->ps1[a] * ff_25v(cpu->ps1[c]))) return;
    (ppc_ps_mul_op)(cpu, d, a, c);
}
FM_INLINE void fm_ps_muls(CPUState* cpu, u8 d, u8 a, u8 c, bool one) {
    f64 cv = one ? cpu->ps1[c] : cpu->fpr[c];
    if (ff_25_ok(cv) && !ff_nan(cv) && ff_pair(cpu, d, cpu->fpr[a] * ff_25v(cv), cpu->ps1[a] * ff_25v(cv))) return;
    if (one) (ppc_ps_muls1)(cpu, d, a, c);
    else (ppc_ps_muls0)(cpu, d, a, c);
}
FM_INLINE void fm_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    if (ff_pair(cpu, d, cpu->fpr[a] + cpu->ps1[b], cpu->ps1[c])) return;
    (ppc_ps_sum0)(cpu, d, a, c, b);
}
FM_INLINE void fm_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    if (ff_pair(cpu, d, cpu->fpr[c], cpu->fpr[a] + cpu->ps1[b])) return;
    (ppc_ps_sum1)(cpu, d, a, c, b);
}
/* One half of a paired multiply-add: as fm_fma (a single-precision a, so
   a*c is exact and a*c + b rounds as fma() does; not a tie). */
FM_INLINE bool ff_madd_in(f64 a, f64 c, f64 b) {
    return ff_finite(a) && (f64)(f32)a == a && fabs(c) < 1e38 && (c == 0.0 || fabs(c) > 1e-200) && ff_finite(b) &&
           ff_25_ok(c);
}
FM_INLINE bool ff_not_tie(f64 r) { return (ff_bits(r) & 0x000000001FFFFFFFull) != 0x0000000010000000ull; }
FM_INLINE void fm_ps_madd(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract, bool negative) {
    if ((cpu->fpscr & 3u) == 0) {
        f64 b0 = subtract ? -cpu->fpr[b] : cpu->fpr[b], b1 = subtract ? -cpu->ps1[b] : cpu->ps1[b];
        if (ff_madd_in(cpu->fpr[a], cpu->fpr[c], b0) && ff_madd_in(cpu->ps1[a], cpu->ps1[c], b1)) {
            f64 r0 = cpu->fpr[a] * ff_25v(cpu->fpr[c]) + b0;
            f64 r1 = cpu->ps1[a] * ff_25v(cpu->ps1[c]) + b1;
            if (ff_not_tie(r0) && ff_not_tie(r1)) {
                f64 s0 = (f64)(f32)r0, s1 = (f64)(f32)r1;
                if (ff_pair(cpu, d, negative ? -s0 : s0, negative ? -s1 : s1)) return;
            }
        }
    }
    (ppc_ps_madd_op)(cpu, d, a, c, b, subtract, negative);
}
FM_INLINE void fm_ps_madds(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool one) {
    f64 cv = one ? cpu->ps1[c] : cpu->fpr[c];
    if ((cpu->fpscr & 3u) == 0 && ff_madd_in(cpu->fpr[a], cv, cpu->fpr[b]) && ff_madd_in(cpu->ps1[a], cv, cpu->ps1[b])) {
        f64 r0 = cpu->fpr[a] * ff_25v(cv) + cpu->fpr[b];
        f64 r1 = cpu->ps1[a] * ff_25v(cv) + cpu->ps1[b];
        if (ff_not_tie(r0) && ff_not_tie(r1) && ff_pair(cpu, d, r0, r1)) return;
    }
    if (one) (ppc_ps_madds1)(cpu, d, a, c, b);
    else (ppc_ps_madds0)(cpu, d, a, c, b);
}

#define ppc_ps_add_op(cpu, d, a, b) fm_ps_add((cpu), (d), (a), (b))
#define ppc_ps_sub_op(cpu, d, a, b) fm_ps_sub((cpu), (d), (a), (b))
#define ppc_ps_mul_op(cpu, d, a, c) fm_ps_mul((cpu), (d), (a), (c))
#define ppc_ps_muls0(cpu, d, a, c) fm_ps_muls((cpu), (d), (a), (c), false)
#define ppc_ps_muls1(cpu, d, a, c) fm_ps_muls((cpu), (d), (a), (c), true)
#define ppc_ps_sum0(cpu, d, a, c, b) fm_ps_sum0((cpu), (d), (a), (c), (b))
#define ppc_ps_sum1(cpu, d, a, c, b) fm_ps_sum1((cpu), (d), (a), (c), (b))
#define ppc_ps_madd_op(cpu, d, a, c, b, subtract, negative) fm_ps_madd((cpu), (d), (a), (c), (b), (subtract), (negative))
#define ppc_ps_madds0(cpu, d, a, c, b) fm_ps_madds((cpu), (d), (a), (c), (b), false)
#define ppc_ps_madds1(cpu, d, a, c, b) fm_ps_madds((cpu), (d), (a), (c), (b), true)

/* Float to integer, toward zero (C's own conversion), within range. */
static inline bool fm_fctiw(CPUState* cpu, f64 v, bool toward_zero, u64* output) {
    if (toward_zero && v > -2147483648.0 && v < 2147483648.0) {
        u32 r = (u32)(s32)v;
        *output = 0xFFF8000000000000ull | r | ((r == 0 && signbit(v)) ? 0x100000000ull : 0ull);
        return true;
    }
    return (ppc_fctiw)(cpu, v, toward_zero, output);
}

#define ppc_fadds(cpu, d, a, b) fm_fadds((cpu), (d), (a), (b))
#define ppc_fsubs(cpu, d, a, b) fm_fsubs((cpu), (d), (a), (b))
#define ppc_fmuls(cpu, d, a, c) fm_fmuls((cpu), (d), (a), (c))
#define ppc_fdivs(cpu, d, a, b) fm_fdivs((cpu), (d), (a), (b))
#define ppc_fadd(cpu, d, a, b) fm_fadd((cpu), (d), (a), (b))
#define ppc_fsub(cpu, d, a, b) fm_fsub((cpu), (d), (a), (b))
#define ppc_fmul(cpu, d, a, c) fm_fmul((cpu), (d), (a), (c))
#define ppc_fdiv(cpu, d, a, b) fm_fdiv((cpu), (d), (a), (b))
#define ppc_frsp(cpu, d, b) fm_frsp((cpu), (d), (b))
#define ppc_fcmp(cpu, crfd, a, b, ordered) fm_fcmp((cpu), (crfd), (a), (b), (ordered))
#define ppc_fctiw(cpu, v, toward_zero, output) fm_fctiw((cpu), (v), (toward_zero), (output))
#define ppc_fma(cpu, a, c, b, single, subtract, negative, output) \
    fm_fma((cpu), (a), (c), (b), (single), (subtract), (negative), (output))
#endif /* NFLS2_NO_FASTFP */

/* The same for the small helpers generated.h defines after including this
   file (these declarations give its definitions the attribute). */
FM_INLINE u32 dolrecomp_f32_to_bits(f64 value);
FM_INLINE f64 dolrecomp_f32_from_bits(u32 bits);
FM_INLINE u64 dolrecomp_f64_to_bits(f64 value);
FM_INLINE f64 dolrecomp_f64_from_bits(u64 bits);
FM_INLINE u32 dolrecomp_rotl32(u32 value, u32 sh);

#endif
