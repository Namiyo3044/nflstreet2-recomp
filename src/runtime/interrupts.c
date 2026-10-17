/* External interrupts: VI retrace timing and delivery to the OS.
 *
 * On hardware an interrupt makes the CPU jump to the vector at 0x80000500,
 * whose code (OSExceptionVector) saves r3-r5, CR, LR, CTR, XER, SRR0 and SRR1
 * into the current OSContext and jumps to the handler registered in the OS
 * exception table. That vector code is never recompiled, so it is done here
 * natively; the handler itself (ExternalInterruptHandler ->
 * __OSDispatchInterrupt -> the VI/DSP/... handlers -> OSLoadContext) is the
 * game's own recompiled code.
 */
#include "runtime/interrupts.h"
#include "runtime/mmio.h"

#include <stdio.h>

#define MSR_EE  0x8000u
#define MSR_PR  0x4000u
#define MSR_FP  0x2000u
#define MSR_FE0 0x0800u
#define MSR_SE  0x0400u
#define MSR_BE  0x0200u
#define MSR_FE1 0x0100u
#define MSR_IR  0x0020u
#define MSR_DR  0x0010u
#define MSR_RI  0x0002u

#define OS_CURRENT_CONTEXT_PHYS 0x800000C0u
#define OS_CURRENT_CONTEXT      0x800000D4u
#define OS_EXCEPTION_TABLE      0x80003000u
#define OS_EXCEPTION_EXTERNAL   4u

/* OSContext offsets */
#define CTX_GPR(n)  ((u32)(n) * 4u)
#define CTX_CR      0x080u
#define CTX_LR      0x084u
#define CTX_CTR     0x088u
#define CTX_XER     0x08Cu
#define CTX_SRR0    0x198u
#define CTX_SRR1    0x19Cu
#define CTX_STATE   0x1A2u
#define CTX_STATE_EXC 0x0002u

/* NTSC: 59.94 fields per second; the timebase runs at 162 MHz / 4. */
#define TIMEBASE_HZ     40500000ull
#define FIELD_TICKS     (TIMEBASE_HZ * 1001ull / 60000ull)

static u64 g_next_field = FIELD_TICKS;
static u32 g_field;
static unsigned long long g_fields, g_taken;

static void take_external_interrupt(CPUState* cpu) {
    u32 ctx = mem_read32(cpu, OS_CURRENT_CONTEXT_PHYS);
    if (ctx < GC_RAM_BASE)
        ctx |= GC_RAM_BASE;    /* stored as a physical address */

    /* What OSExceptionVector saves. */
    mem_write32(cpu, ctx + CTX_GPR(3), cpu->gpr[3]);
    mem_write32(cpu, ctx + CTX_GPR(4), cpu->gpr[4]);
    mem_write32(cpu, ctx + CTX_GPR(5), cpu->gpr[5]);
    mem_write16(cpu, ctx + CTX_STATE, (u16)(mem_read16(cpu, ctx + CTX_STATE) | CTX_STATE_EXC));
    mem_write32(cpu, ctx + CTX_CR, cpu->cr);
    mem_write32(cpu, ctx + CTX_LR, cpu->lr);
    mem_write32(cpu, ctx + CTX_CTR, cpu->ctr);
    mem_write32(cpu, ctx + CTX_XER, cpu->xer);
    mem_write32(cpu, ctx + CTX_SRR0, cpu->pc);   /* resume at the next instruction */
    mem_write32(cpu, ctx + CTX_SRR1, cpu->msr);

    /* The CPU's exception entry state, plus the vector turning translation on. */
    cpu->srr0 = cpu->pc;
    cpu->srr1 = cpu->msr;
    cpu->msr = (cpu->msr & ~(MSR_EE | MSR_PR | MSR_FP | MSR_FE0 | MSR_FE1 | MSR_SE | MSR_BE | MSR_RI))
               | MSR_IR | MSR_DR;

    cpu->gpr[3] = OS_EXCEPTION_EXTERNAL;
    cpu->gpr[4] = mem_read32(cpu, OS_CURRENT_CONTEXT);
    cpu->pc = mem_read32(cpu, OS_EXCEPTION_TABLE + OS_EXCEPTION_EXTERNAL * 4u);
    g_taken++;
}

void interrupts_poll(CPUState* cpu) {
    /* Video: one display interrupt per field, alternating the two that VIInit
       enables (DI1 at the top of the frame, DI0 at the field boundary). */
    while (cpu->timebase >= g_next_field) {
        g_next_field += FIELD_TICKS;
        mmio_vi_raise(g_field ? 0u : 1u);
        mmio_si_field();
        g_field ^= 1u;
        g_fields++;
    }

    if (!(cpu->msr & MSR_EE))
        return;
    u32 pending = mmio_pi_cause() & mmio_pi_mask() & ~0x00010000u;
    if (pending && mem_read32(cpu, OS_EXCEPTION_TABLE + OS_EXCEPTION_EXTERNAL * 4u))
        take_external_interrupt(cpu);
}

double interrupts_game_seconds(void) {
    return (double)g_fields * 1001.0 / 60000.0;
}

void interrupts_report(void) {
    printf("video fields: %llu (%.1f s of game time), external interrupts taken: %llu\n",
           g_fields, (double)g_fields * 1001.0 / 60000.0, g_taken);
}
