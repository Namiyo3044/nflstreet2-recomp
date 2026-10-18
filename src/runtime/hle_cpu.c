#include "hle_cpu.h"
#include "runtime/mmio.h"

#include <stdio.h>
#include <string.h>

/* Backing store for supervisor SPRs with no real meaning on a PC: BATs, HID0/1,
   L2CR, performance counters, thermal sensors, DMA, WPAR and so on. Writes are
   remembered and read back so the game sees a consistent machine. */
static u32 g_spr[1024];
static u32 g_spr_reads[1024];
static u32 g_spr_writes[1024];
static u32 g_cache_ops[4];
static u32 g_unhandled;

static u32 spr_read(CPUState* cpu, u16 spr, u32 cia) {
    (void)cpu;
    (void)cia;
    g_spr_reads[spr & 1023]++;
    switch (spr) {
    case 287:  /* PVR */
        return PPC_GEKKO_PVR;
    case 22:   /* DEC: count down with the timebase so timer loops progress */
        return g_spr[22] - (u32)cpu->timebase;
    default:
        return g_spr[spr & 1023];
    }
}

static void spr_write(CPUState* cpu, u16 spr, u32 value, u32 cia) {
    (void)cia;
    g_spr_writes[spr & 1023]++;
    if (spr == 22) {
        g_spr[22] = value + (u32)cpu->timebase;
        return;
    }
    if (spr == 923 && (value & 2u)) {
        /* DMAL with the trigger bit: run the locked-cache DMA now. Length in
           32-byte lines: DMAU bits 0-4 high, DMAL bits 2-3 low; 0 = 128. */
        u32 dmau = g_spr[922];
        u32 lines = ((dmau & 0x1Fu) << 2) | ((value >> 2) & 3u);
        if (!lines) lines = 128;
        mmio_locked_cache_dma(cpu, dmau & ~0x1Fu, value & ~0x1Fu, lines * 32u, (value & 0x10u) != 0);
        value &= ~2u;                  /* done: the trigger bit reads back clear */
    }
    g_spr[spr & 1023] = value;
    if (spr == 921)
        mmio_wpar_written();           /* resets the write-gather pipe */
}

static void cache_control(CPUState* cpu, u8 operation, u32 ea, u32 cia) {
    (void)cpu;
    (void)ea;
    (void)cia;
    /* There is no separate instruction/data cache to keep coherent on a PC. */
    if (operation < 4)
        g_cache_ops[operation]++;
}

static void instruction_fallback(CPUState* cpu, u32 raw, u32 cia) {
    u32 op = raw >> 26;
    u32 xo = (raw >> 1) & 0x3FFu;
    u32 rD = (raw >> 21) & 31u;
    u32 rA = (raw >> 16) & 31u;
    u32 rB = (raw >> 11) & 31u;
    u16 spr = (u16)(((raw >> 16) & 0x1Fu) | (((raw >> 11) & 0x1Fu) << 5));
    u32 ea = (rA ? cpu->gpr[rA] : 0u) + cpu->gpr[rB];

    if (op == 31) {
        switch (xo) {
        case 339: /* mfspr */
            cpu->gpr[rD] = ppc_mfspr(cpu, spr, cia);
            break;
        case 467: /* mtspr */
            ppc_mtspr(cpu, spr, cpu->gpr[rD], cia);
            break;
        case 371: /* mftb */
            cpu->gpr[rD] = ppc_mftb(cpu, spr, cia);
            break;
        case 54:  ppc_cache_control(cpu, PPC_CACHE_DCBST, ea, cia); break;
        case 86:  ppc_cache_control(cpu, PPC_CACHE_DCBF, ea, cia);  break;
        case 470: ppc_cache_control(cpu, PPC_CACHE_DCBI, ea, cia);  break;
        case 982: ppc_cache_control(cpu, PPC_CACHE_ICBI, ea, cia);  break;
        case 1014: /* dcbz: zero one 32-byte cache line */
            for (u32 i = 0; i < 32; i += 4)
                mem_write32(cpu, (ea & ~31u) + i, 0);
            break;
        case 598: /* sync */
        case 854: /* eieio */
            break;
        default:
            goto unhandled;
        }
    } else if (op == 19 && xo == 150) {
        /* isync */
    } else {
        goto unhandled;
    }

    if (!cpu->exception)
        cpu->pc = cia + 4u;
    return;

unhandled:
    g_unhandled++;
    fprintf(stderr, "unhandled instruction 0x%08X at 0x%08X (op %u, xo %u)\n", raw, cia, op, xo);
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

void hle_cpu_install(CPUState* cpu) {
    memset(g_spr, 0, sizeof(g_spr));
    cpu->spr_read = spr_read;
    cpu->spr_write = spr_write;
    cpu->cache_control = cache_control;
    cpu->instruction_fallback = instruction_fallback;
}

void hle_cpu_report(void) {
    static const struct { u16 spr; const char* name; } named[] = {
        {22, "DEC"}, {272, "SPRG0"}, {273, "SPRG1"}, {274, "SPRG2"}, {275, "SPRG3"},
        {528, "IBAT0U"}, {536, "DBAT0U"}, {921, "WPAR"}, {922, "DMAU"}, {923, "DMAL"},
        {952, "MMCR0"}, {1008, "HID0"}, {1009, "HID1"}, {1017, "L2CR"},
    };
    printf("supervisor registers touched:");
    int any = 0;
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        u16 s = named[i].spr;
        if (g_spr_reads[s] || g_spr_writes[s]) {
            printf(" %s(r%u/w%u)", named[i].name, g_spr_reads[s], g_spr_writes[s]);
            any = 1;
        }
    }
    printf("%s\n", any ? "" : " none");
    printf("cache ops: dcbst %u, dcbf %u, dcbi %u, icbi %u\n",
           g_cache_ops[PPC_CACHE_DCBST], g_cache_ops[PPC_CACHE_DCBF],
           g_cache_ops[PPC_CACHE_DCBI], g_cache_ops[PPC_CACHE_ICBI]);
    if (g_unhandled)
        printf("unhandled instructions: %u\n", g_unhandled);
}
