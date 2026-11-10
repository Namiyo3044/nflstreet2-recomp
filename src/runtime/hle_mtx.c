/* Native versions of the game's hottest small library functions: the
 * paired-single matrix routines from Nintendo's SDK (PSMTXConcat,
 * PSMTXMultVec, PSMTXMultVecSR), which skinning and the camera call tens of
 * thousands of times a frame. Run through the recompiled code, each guest
 * instruction is checked and worked out on its own (FP enabled? quantisation
 * type? in RAM? a NaN?), so these few dozen instructions came to about a
 * tenth of all the game code's time.
 *
 * Each version here does the whole function at once: the same arithmetic,
 * in the same order, as the recompiled code's fast paths (fastmem.h), so
 * the results (registers, memory, the cycle count) are the same bit for
 * bit; it checks once, up front, what makes every one of those fast paths
 * apply (see in_range below). If anything doesn't (FP off, quantised GQR0,
 * an address outside memory or unaligned, a value out of range, the output
 * overlapping what is still to be read) it changes nothing and returns
 * false, and the recompiled code runs instead, as before.
 * (tests/hle_mtx_test.c checks it against the recompiled code.)
 *
 * Called from the game code's direct calls (tools/directcalls.c emits the
 * call for the addresses in its list), so a call the port hooks, or every
 * call while a debug trace is on, still goes the normal way.
 */
#include "runtime/fastmem.h"
#include "runtime/hle_mtx.h"
#include <stdio.h>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

int g_nfls2_hle_on = 1;
unsigned long long g_nfls2_hle_calls, g_nfls2_hle_declined;

/* Why a call was left to the game code (the end-of-run report). */
enum { WHY_FP_OFF = 1, WHY_SETUP, WHY_ADDRESS, WHY_OVERLAP, WHY_RANGE, WHY_NAN, WHY_N };
static const char* const k_why[WHY_N] = {"", "FP off", "GQR / HID2 / FPSCR / reservation", "address", "overlap",
                                         "value out of range", "NaN in a register"};
static unsigned long long g_calls[3], g_why[3][WHY_N];

typedef struct { f64 a, b; } PS;

/* (the conditions every psq_l / psq_st and ps_madd in these functions
   needs: FP on, paired singles on, GQR0 plain floats, no lwarx reservation,
   round to nearest) */
static int ps_ready(CPUState* cpu) {
    if (!(cpu->msr & 0x00002000u)) return WHY_FP_OFF;
    if (!fm_psq_ok(cpu, false) || ((cpu->gqr[0] >> 16) & 7u) != 0 || (cpu->gqr[0] & 7u) != 0 || cpu->reserve_valid ||
        (cpu->fpscr & 3u) != 0)
        return WHY_SETUP;
    return 0;
}

/* Why the arithmetic below needs so few checks: every value loaded is
 * required to be a single below 2^62 in size (zero, denormals and tiny
 * values included; anything else, infinities and NaNs too: the recompiled
 * code instead). Then, as the fast paths' conditions ask:
 *  - a product of two singles (one rounded to 25 bits) is exact in a
 *    double, however small;
 *  - each multiply-add's a and c are loaded values (singles, so below 1e38
 *    and, unless zero, above 1e-200) and its b is a result, so only a
 *    rounding tie can send it to the full version (worked out here the
 *    same way: fma_r);
 *  - products stay below 2^124 and sums of four below 2^126: no infinity,
 *    no NaN anywhere;
 *  - a result too small for a normal single is rounded the way the fast
 *    paths do it (a plain cast), and a store of one writes 0, as the CPU
 *    core's psq_st does (the fast path leaves it to the core).
 * The only other inputs, two registers' second halves that ps_sum0 carries
 * through in PSMTXMultVec, are checked for NaN. */
FM_INLINE bool in_range(u32 x) { return ((x >> 23) & 0xFFu) <= 127u + 61u; }

FM_INLINE f64 f32v(u32 x) {
    f32 f;
    memcpy(&f, &x, 4);
    return (f64)f;
}

FM_INLINE u32 be32(const u8* p) {
    u32 x;
    memcpy(&x, p, 4);
    return FM_BSWAP32(x);
}

/* psq_l, w=0 / w=1 (the address checked by the caller) */
FM_INLINE PS ld2(const u8* p, bool* ok) {
    u32 x = be32(p), y = be32(p + 4);
    *ok &= (int)in_range(x) & (int)in_range(y);
    PS r = {f32v(x), f32v(y)};
    return r;
}
FM_INLINE PS ld1(const u8* p, bool* ok) {
    u32 x = be32(p);
    *ok &= in_range(x);
    PS r = {f32v(x), 1.0};
    return r;
}

FM_INLINE f64 rs(f64 v) { return (f64)(f32)v; }

FM_INLINE PS muls(PS a, f64 cv) {          /* ps_muls0 / ps_muls1 */
    f64 c = ff_25v(cv);
    PS r = {rs(a.a * c), rs(a.b * c)};
    return r;
}
FM_INLINE PS mul(PS a, PS c) {             /* ps_mul */
    PS r = {rs(a.a * ff_25v(c.a)), rs(a.b * ff_25v(c.b))};
    return r;
}
/* A multiply-add's result as a double, and the CPU core's correction when
 * it lands exactly halfway between two singles (correct_single_fma_tie:
 * whether the exact sum was a little above or below decides; the product
 * p is exact, so p + a_prime is what its fma() gives). The recompiled code
 * sends such an operation to the core, which also sets the FPSCR's result
 * class (FPRF) from the first half: *fprf keeps the last one. */
FM_INLINE f64 fma_r(f64 p, f64 addend, bool* tie) {
    f64 r = p + addend;
    if (ff_not_tie(r)) return r;
    *tie = true;
    f64 ap = addend - r, bp = r + ap;
    f64 err = (p + ap) + (addend - bp);
    if (err == 0.0) return r;
    u64 bits = ff_bits(r);
    return ff_value((err > 0.0) == (r > 0.0) ? bits + 1u : bits - 1u);
}
FM_INLINE void note_fprf(int* fprf, f64 ps0) {
    f32 f = (f32)ps0;
    u32 x;
    memcpy(&x, &f, 4);
    /* (classify_f32 for what can occur here: normal, denormal or zero) */
    u32 neg = x >> 31, e = x & 0x7F800000u;
    if (e) *fprf = neg ? 0x08 : 0x04;
    else if (x & 0x007FFFFFu) *fprf = neg ? 0x18 : 0x14;
    else *fprf = neg ? 0x12 : 0x02;
}
FM_INLINE PS madds(PS a, f64 cv, PS b, int* fprf) {  /* ps_madds0 / ps_madds1 */
    f64 c = ff_25v(cv);
    bool tie = false;
    f64 r0 = fma_r(a.a * c, b.a, &tie), r1 = fma_r(a.b * c, b.b, &tie);
    PS r = {rs(r0), rs(r1)};
    if (tie) note_fprf(fprf, r0);
    return r;
}
FM_INLINE PS madd(PS a, PS c, PS b, int* fprf) {     /* ps_madd */
    bool tie = false;
    f64 r0 = fma_r(a.a * ff_25v(c.a), b.a, &tie), r1 = fma_r(a.b * ff_25v(c.b), b.b, &tie);
    PS r = {rs(r0), rs(r1)};
    if (tie) note_fprf(fprf, r0);
    return r;
}
FM_INLINE PS sum0(PS a, PS c, PS b) {      /* ps_sum0 d, a, c, b */
    PS r = {rs(a.a + b.b), rs(c.b)};
    return r;
}

FM_INLINE u32 st32(f64 v) {               /* psq_st's conversion (a denormal: 0, as the core stores it) */
    f32 f = (f32)v;
    u32 x;
    memcpy(&x, &f, 4);
    if (!fm_f32_store_ok(x)) x = 0;
    return FM_BSWAP32(x);
}

/* Where n bytes at ea live, as the CPU core would find them: main RAM, its
   uncached mirror (0xC0000000), or the locked cache (0xE0000000; the
   game's skinning works there); NULL for anything else or unaligned. */
u8* mmio_l1_at(u32 ea, u32 n);
static u8* host_at(CPUState* cpu, u32 ea, u32 n) {
    if (ea & 3u) return NULL;
    u32 off;
    if (FM_RAM(cpu, ea, n, off)) return cpu->ram + off;
    if ((off = ea - 0xC0000000u) <= cpu->ram_size - n) return cpu->ram + off;
    if ((ea & 0xF0000000u) == 0xE0000000u) return mmio_l1_at(ea, n);
    return NULL;
}

FM_INLINE PS reg(CPUState* cpu, int i) {
    PS p = {cpu->fpr[i], cpu->ps1[i]};
    return p;
}
FM_INLINE void set(CPUState* cpu, int i, PS p) {
    cpu->fpr[i] = p.a;
    cpu->ps1[i] = p.b;
}

/* (compared where they live, so a mirror or the cache is caught too) */
FM_INLINE bool overlap(const u8* a, u32 an, const u8* b, u32 bn) { return a < b + bn && b < a + an; }

/* PSMTXConcat(a r3, b r4, ab r5): 51 instructions; saves f14, f15, f31 in
   a 64-byte frame and loads the constant pair at 0x803EC250 into f31.
   Every load comes before the first store to ab, so ab may be a or b. */
#define MTX_CONCAT_UNIT 0x803EC250u
static int mtx_concat(CPUState* cpu) {
    u32 r1 = cpu->gpr[1], ea = cpu->gpr[3], eb = cpu->gpr[4], eab = cpu->gpr[5], sp = r1 - 64u;
    u8 *A = host_at(cpu, ea, 48u), *B = host_at(cpu, eb, 48u), *AB = host_at(cpu, eab, 48u);
    u8 *S = host_at(cpu, sp, 48u), *U = host_at(cpu, MTX_CONCAT_UNIT, 8u);
    if (!A || !B || !AB || !S || !U) return WHY_ADDRESS;
    if (overlap(A, 48u, S, 48u) || overlap(B, 48u, S, 48u) || overlap(AB, 48u, S, 48u)) return WHY_OVERLAP;
    bool rng = true;
    int fprf = -1;
    PS f0 = ld2(A, &rng), f6 = ld2(B, &rng), f7 = ld2(B + 8, &rng), f8 = ld2(B + 16, &rng);
    PS f12 = muls(f6, f0.a), f2 = ld2(A + 16, &rng), f13 = muls(f7, f0.a), f31 = ld2(U, &rng);
    PS f14 = muls(f6, f2.a), f9 = ld2(B + 24, &rng), f15 = muls(f7, f2.a), f1 = ld2(A + 8, &rng);
    f12 = madds(f8, f0.b, f12, &fprf);
    PS f3 = ld2(A + 24, &rng);
    f14 = madds(f8, f2.b, f14, &fprf);
    PS f10 = ld2(B + 32, &rng);
    f13 = madds(f9, f0.b, f13, &fprf);
    PS f11 = ld2(B + 40, &rng);
    f15 = madds(f9, f2.b, f15, &fprf);
    PS f4 = ld2(A + 32, &rng), f5 = ld2(A + 40, &rng);
    f12 = madds(f10, f1.a, f12, &fprf);
    f13 = madds(f11, f1.a, f13, &fprf);
    f14 = madds(f10, f3.a, f14, &fprf);
    f15 = madds(f11, f3.a, f15, &fprf);
    PS s12 = f12;                                   /* psq_st f12, 0(r5) */
    f2 = muls(f6, f4.a);
    f13 = madds(f31, f1.b, f13, &fprf);
    PS f0n = muls(f7, f4.a);
    PS s14 = f14;                                   /* psq_st f14, 16(r5) */
    f15 = madds(f31, f3.b, f15, &fprf);
    PS s13 = f13;                                   /* psq_st f13, 8(r5) */
    f2 = madds(f8, f4.b, f2, &fprf);
    f0n = madds(f9, f4.b, f0n, &fprf);
    f2 = madds(f10, f5.a, f2, &fprf);
    PS s15 = f15;                                   /* psq_st f15, 24(r5) */
    f0n = madds(f11, f5.a, f0n, &fprf);
    PS s2 = f2;                                     /* psq_st f2, 32(r5) */
    f0n = madds(f31, f5.b, f0n, &fprf);
    PS s0 = f0n;                                    /* psq_st f0, 40(r5) */
    u32 w[12] = {st32(s12.a), st32(s12.b), st32(s13.a), st32(s13.b),
                 st32(s14.a), st32(s14.b), st32(s15.a), st32(s15.b),
                 st32(s2.a),  st32(s2.b),  st32(s0.a),  st32(s0.b)};
    if (!rng) return WHY_RANGE;
    /* Done: the state as the instructions leave it. */
    u32 bc = FM_BSWAP32(r1);
    memcpy(S, &bc, 4);                                          /* stwu r1, -64(r1) */
    u64 d14 = FM_BSWAP64(ff_bits(cpu->fpr[14])), d15 = FM_BSWAP64(ff_bits(cpu->fpr[15])),
        d31 = FM_BSWAP64(ff_bits(cpu->fpr[31]));
    memcpy(S + 8, &d14, 8);                                     /* stfd f14, 8(r1) */
    memcpy(S + 16, &d15, 8);                                    /* stfd f15, 16(r1) */
    memcpy(S + 40, &d31, 8);                                    /* stfd f31, 40(r1) */
    memcpy(AB, w, 48);                             /* (no overlap with the frame; all loads done) */
    f14.a = cpu->fpr[14];                          /* lfd f14 / f15 / f31: the first halves back */
    f15.a = cpu->fpr[15];
    f31.a = cpu->fpr[31];
    set(cpu, 0, f0n); set(cpu, 1, f1); set(cpu, 2, f2); set(cpu, 3, f3); set(cpu, 4, f4); set(cpu, 5, f5);
    set(cpu, 6, f6); set(cpu, 7, f7); set(cpu, 8, f8); set(cpu, 9, f9); set(cpu, 10, f10); set(cpu, 11, f11);
    set(cpu, 12, f12); set(cpu, 13, f13); set(cpu, 14, f14); set(cpu, 15, f15); set(cpu, 31, f31);
    cpu->gpr[6] = MTX_CONCAT_UNIT;
    if (fprf >= 0) cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | ((u32)fprf << 12);
    cpu->downcount -= 51;
    return 0;
}

/* PSMTXMultVec(m r3, src r4, dst r5): 21 instructions. Its first store
   comes before the last two loads from m, so dst mustn't overlap m. */
static int mtx_multvec(CPUState* cpu) {
    u32 em = cpu->gpr[3], ev = cpu->gpr[4], ed = cpu->gpr[5];
    u8 *M = host_at(cpu, em, 48u), *V = host_at(cpu, ev, 12u), *D = host_at(cpu, ed, 12u);
    if (!M || !V || !D) return WHY_ADDRESS;
    if (overlap(D, 12u, M, 48u)) return WHY_OVERLAP;
    PS f6 = reg(cpu, 6), f12 = reg(cpu, 12);
    if (ff_nan(f6.b) || ff_nan(f12.b)) return WHY_NAN;
    bool rng = true;
    int fprf = -1;
    PS f0 = ld2(V, &rng), f2 = ld2(M, &rng), f1 = ld1(V + 8, &rng);
    PS f4 = mul(f2, f0), f3 = ld2(M + 8, &rng);
    PS f5 = madd(f3, f1, f4, &fprf);
    PS f8 = ld2(M + 16, &rng);
    f6 = sum0(f5, f6, f5);
    PS f9 = ld2(M + 24, &rng), f10 = mul(f8, f0);
    u32 x = st32(f6.a);                        /* psq_st f6, 0(r5), w=1 */
    PS f11 = madd(f9, f1, f10, &fprf);
    f2 = ld2(M + 32, &rng);
    f12 = sum0(f11, f12, f11);
    f3 = ld2(M + 40, &rng);
    f4 = mul(f2, f0);
    u32 y = st32(f12.a);                       /* psq_st f12, 4(r5), w=1 */
    f5 = madd(f3, f1, f4, &fprf);
    f6 = sum0(f5, f6, f5);
    u32 z = st32(f6.a);                        /* psq_st f6, 8(r5), w=1 */
    if (!rng) return WHY_RANGE;
    memcpy(D, &x, 4);
    memcpy(D + 4, &y, 4);
    memcpy(D + 8, &z, 4);
    set(cpu, 0, f0); set(cpu, 1, f1); set(cpu, 2, f2); set(cpu, 3, f3); set(cpu, 4, f4); set(cpu, 5, f5);
    set(cpu, 6, f6); set(cpu, 8, f8); set(cpu, 9, f9); set(cpu, 10, f10); set(cpu, 11, f11); set(cpu, 12, f12);
    if (fprf >= 0) cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | ((u32)fprf << 12);
    cpu->downcount -= 21;
    return 0;
}

/* PSMTXMultVecSR(m r3, src r4, dst r5): 21 instructions, every load
   before the first store. */
static int mtx_multvec_sr(CPUState* cpu) {
    u32 em = cpu->gpr[3], ev = cpu->gpr[4], ed = cpu->gpr[5];
    u8 *M = host_at(cpu, em, 48u), *V = host_at(cpu, ev, 12u), *D = host_at(cpu, ed, 12u);
    if (!M || !V || !D) return WHY_ADDRESS;
    bool rng = true;
    int fprf = -1;
    PS f0 = ld2(M, &rng), f6 = ld2(V, &rng), f2 = ld2(M + 16, &rng);
    PS f8 = mul(f0, f6), f4 = ld2(M + 32, &rng), f10 = mul(f2, f6), f7 = ld1(V + 8, &rng);
    PS f12 = mul(f4, f6), f3 = ld2(M + 24, &rng);
    f8 = sum0(f8, f8, f8);
    PS f5 = ld2(M + 40, &rng);
    f10 = sum0(f10, f10, f10);
    PS f1 = ld2(M + 8, &rng);
    f12 = sum0(f12, f12, f12);
    PS f9 = madd(f1, f7, f8, &fprf);
    PS f11 = madd(f3, f7, f10, &fprf);
    PS f13 = madd(f5, f7, f12, &fprf);
    u32 x = st32(f9.a), y = st32(f11.a), z = st32(f13.a);
    if (!rng) return WHY_RANGE;
    memcpy(D, &x, 4);
    memcpy(D + 4, &y, 4);
    memcpy(D + 8, &z, 4);
    set(cpu, 0, f0); set(cpu, 1, f1); set(cpu, 2, f2); set(cpu, 3, f3); set(cpu, 4, f4); set(cpu, 5, f5);
    set(cpu, 6, f6); set(cpu, 7, f7); set(cpu, 8, f8); set(cpu, 9, f9); set(cpu, 10, f10); set(cpu, 11, f11);
    set(cpu, 12, f12); set(cpu, 13, f13);
    if (fprf >= 0) cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | ((u32)fprf << 12);
    cpu->downcount -= 21;
    return 0;
}

int nfls2_hle(CPUState* cpu, u32 target) {
    if (!g_nfls2_hle_on)
        return 0;
    int f = target == NFLS2_HLE_MTX_CONCAT ? 0 : target == NFLS2_HLE_MTX_MULTVEC ? 1 : 2;
    g_calls[f]++;
    int why = ps_ready(cpu);
    if (!why) {
        switch (target) {
        case NFLS2_HLE_MTX_CONCAT: why = mtx_concat(cpu); break;
        case NFLS2_HLE_MTX_MULTVEC: why = mtx_multvec(cpu); break;
        default: why = mtx_multvec_sr(cpu); break;
        }
    }
    if (why) {
        g_why[f][why]++;
        g_nfls2_hle_declined++;
        return 0;
    }
    g_nfls2_hle_calls++;
    cpu->pc = cpu->lr & ~3u;                       /* blr */
    return 1;
}

void nfls2_hle_report(void) {
    static const char* const names[3] = {"PSMTXConcat", "PSMTXMultVec", "PSMTXMultVecSR"};
    if (!g_nfls2_hle_calls && !g_nfls2_hle_declined)
        return;
    printf("native matrix functions: %llu calls done natively, %llu left to the game code\n", g_nfls2_hle_calls,
           g_nfls2_hle_declined);
    for (int f = 0; f < 3; f++) {
        if (!g_calls[f]) continue;
        printf("  %-15s %10llu calls", names[f], g_calls[f]);
        for (int w = 1; w < WHY_N; w++)
            if (g_why[f][w]) printf(", %s %.1f%%", k_why[w], 100.0 * (double)g_why[f][w] / (double)g_calls[f]);
        printf("\n");
    }
}
