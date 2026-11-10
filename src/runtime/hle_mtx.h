#ifndef NFLS2_HLE_MTX_H
#define NFLS2_HLE_MTX_H

#include "cpu/cpu.h"

/* Native versions of a few hot SDK functions (see hle_mtx.c). The addresses are
   also in tools/directcalls.c's list. */
#define NFLS2_HLE_MTX_CONCAT     0x8024AA18u   /* PSMTXConcat */
#define NFLS2_HLE_MTX_MULTVEC    0x8024AAE4u   /* PSMTXMultVec */
#define NFLS2_HLE_MTX_MULTVEC_SR 0x8024AB38u   /* PSMTXMultVecSR */

/* 1 if it ran the whole function (ctx->pc is then the return address), 0
   to leave it to the game's code. */
int nfls2_hle(CPUState* cpu, u32 target);

extern int g_nfls2_hle_on;                     /* --no-native-matrix turns it off */
extern unsigned long long g_nfls2_hle_calls, g_nfls2_hle_declined;
void nfls2_hle_report(void);

#endif
