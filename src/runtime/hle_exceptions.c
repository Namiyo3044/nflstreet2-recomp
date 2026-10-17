/* CPU exceptions the game expects the OS's exception vectors to handle.
 *
 * On hardware the OS copies small handlers into low memory (0x80000000-
 * 0x80003000) and the CPU jumps there. That code is never recompiled, so the
 * runner calls these native equivalents instead.
 */
#include "generated.h"
#include "runtime/hle_exceptions.h"

#include <stdio.h>

#define MSR_FP 0x00002000u

/* Low-memory OS globals (see the SDK's OSContext.c). */
#define OS_CURRENT_CONTEXT 0x800000D4u
#define OS_FPU_CONTEXT     0x800000D8u

/* OSContext field offsets. */
#define CTX_FPR    0x090u
#define CTX_FPSCR  0x194u
#define CTX_STATE  0x1A2u
#define CTX_PSF    0x1C8u
#define CTX_STATE_FPSAVED 0x0001u
#define CTX_STATE_EXC     0x0002u

static unsigned long long g_syscalls, g_fpu_switches;

static void save_fpu(CPUState* cpu, u32 ctx) {
    mem_write16(cpu, ctx + CTX_STATE, (u16)(mem_read16(cpu, ctx + CTX_STATE) | CTX_STATE_FPSAVED));
    for (u32 i = 0; i < 32; i++)
        mem_write64(cpu, ctx + CTX_FPR + i * 8u, dolrecomp_f64_to_bits(cpu->fpr[i]));
    mem_write32(cpu, ctx + CTX_FPSCR - 4u, 0);
    mem_write32(cpu, ctx + CTX_FPSCR, cpu->fpscr);
    if (cpu->hid2 & PPC_HID2_PSE) {
        for (u32 i = 0; i < 32; i++) {
            mem_write32(cpu, ctx + CTX_PSF + i * 8u, dolrecomp_ps_to_bits(cpu->fpr[i]));
            mem_write32(cpu, ctx + CTX_PSF + i * 8u + 4u, dolrecomp_ps_to_bits(cpu->ps1[i]));
        }
    }
}

static void load_fpu(CPUState* cpu, u32 ctx) {
    if (!(mem_read16(cpu, ctx + CTX_STATE) & CTX_STATE_FPSAVED))
        return;
    cpu->fpscr = mem_read32(cpu, ctx + CTX_FPSCR);
    ppc_fpscr_updated(cpu);
    if (cpu->hid2 & PPC_HID2_PSE) {
        for (u32 i = 0; i < 32; i++)
            cpu->ps1[i] = dolrecomp_ps_from_bits(mem_read32(cpu, ctx + CTX_PSF + i * 8u + 4u));
    }
    for (u32 i = 0; i < 32; i++)
        cpu->fpr[i] = dolrecomp_f64_from_bits(mem_read64(cpu, ctx + CTX_FPR + i * 8u));
}

bool hle_handle_exception(CPUState* cpu) {
    if (cpu->exception == PPC_EXC_SYSTEM_CALL) {
        /* The SDK's only system call is the "sync" at the end of DCFlushRange
           and friends. Nothing to sync on a PC: return past the sc. */
        cpu->exception = 0;
        ppc_rfi(cpu, cpu->pc);
        g_syscalls++;
        return true;
    }

    if (cpu->exception == PPC_EXC_FP_UNAVAILABLE) {
        /* Lazy FPU switching (OSSwitchFPUContext): threads start with FP
           disabled; the first FP instruction lands here, and the FP registers
           are saved for the thread that owned them and loaded for this one. */
        u32 current = mem_read32(cpu, OS_CURRENT_CONTEXT);
        u32 owner = mem_read32(cpu, OS_FPU_CONTEXT);
        mem_write32(cpu, OS_FPU_CONTEXT, current);
        if (owner != current) {
            if (owner)
                save_fpu(cpu, owner);
            if (current)
                load_fpu(cpu, current);
        }
        if (current)
            mem_write16(cpu, current + CTX_STATE,
                        (u16)(mem_read16(cpu, current + CTX_STATE) & ~CTX_STATE_EXC));

        cpu->exception = 0;
        ppc_rfi(cpu, cpu->pc);
        cpu->msr |= MSR_FP;   /* the handler sets FP in SRR1 before rfi */
        g_fpu_switches++;
        return true;
    }

    return false;
}

void hle_exceptions_report(void) {
    printf("system calls (sync) handled: %llu, FPU context switches: %llu\n",
           g_syscalls, g_fpu_switches);
}
