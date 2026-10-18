#include "runtime/interp.h"

#include <stdio.h>
#include <string.h>

/* A small PowerPC interpreter for code the game writes into memory at run
 * time. The recompiler only translated main.dol, so such code has no native
 * version. NFL Street 2's sound engine builds short call stubs ("thunks") in
 * the heap that save registers, call an engine function and return; this
 * runs them. It covers the integer, load/store and branch instructions such
 * stubs use; anything else stops the game with a message naming it, so it
 * can be added.
 *
 * Execution stops at the first taken branch (or after a few thousand
 * instructions): the main loop then dispatches the new pc, which is either
 * recompiled code or more run-time code for another round here.
 */

static unsigned long long g_runs, g_insns;
static u32 g_first_pc, g_last_unknown, g_unknown_pc;

static u32 rotl32(u32 v, u32 n) {
    n &= 31u;
    return n ? (v << n) | (v >> (32u - n)) : v;
}

static u32 mask32(u32 mb, u32 me) {
    u32 m = mb <= me ? (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31u - me))
                     : (0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31u - me));
    return m;
}

static void set_cr_field(CPUState* cpu, u32 field, u32 bits) {
    u32 shift = 28u - field * 4u;
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | ((bits & 0xFu) << shift);
}

static void cmp_signed(CPUState* cpu, u32 field, s32 a, s32 b) {
    u32 bits = a < b ? 8u : a > b ? 4u : 2u;
    set_cr_field(cpu, field, bits | ((cpu->xer >> 31) & 1u));
}

static void cmp_unsigned(CPUState* cpu, u32 field, u32 a, u32 b) {
    u32 bits = a < b ? 8u : a > b ? 4u : 2u;
    set_cr_field(cpu, field, bits | ((cpu->xer >> 31) & 1u));
}

static void record(CPUState* cpu, u32 v) {
    cmp_signed(cpu, 0, (s32)v, 0);
}

static bool cond_ok(CPUState* cpu, u32 bo, u32 bi) {
    if (!(bo & 4u))
        cpu->ctr--;
    bool ctr_ok = (bo & 4u) || ((cpu->ctr != 0) ^ ((bo >> 1) & 1u));
    bool cond = (bo & 16u) || (((cpu->cr >> (31u - bi)) & 1u) == ((bo >> 3) & 1u));
    return ctr_ok && cond;
}

static f64 f32_to_f64(u32 bits) {
    f32 f;
    memcpy(&f, &bits, 4);
    return (f64)f;
}

static u32 f64_to_f32_bits(f64 v) {
    f32 f = (f32)v;
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}

InterpResult interp_run(CPUState* cpu, u32 max_insns) {
    g_runs++;
    if (!g_first_pc)
        g_first_pc = cpu->pc;
    for (u32 n = 0; n < max_insns; n++) {
        u32 cia = cpu->pc;
        if (cia < 0x80000000u || cia - 0x80000000u >= cpu->ram_size || (cia & 3u))
            return INTERP_BAD_ADDRESS;
        u32 raw = mem_read32(cpu, cia);
        u32 op = raw >> 26;
        u32 rD = (raw >> 21) & 31u, rA = (raw >> 16) & 31u, rB = (raw >> 11) & 31u;
        s32 simm = (s16)(raw & 0xFFFFu);
        u32 uimm = raw & 0xFFFFu;
        u32 ra0 = rA ? cpu->gpr[rA] : 0u;
        u32 next = cia + 4u;
        bool taken = false;
        cpu->downcount--;
        g_insns++;

        switch (op) {
        case 7:  cpu->gpr[rD] = (u32)((s32)cpu->gpr[rA] * simm); break;            /* mulli */
        case 8: {                                                                   /* subfic */
            u32 a = cpu->gpr[rA];
            cpu->gpr[rD] = (u32)simm - a;
            if ((u32)simm >= a) cpu->xer |= 0x20000000u; else cpu->xer &= ~0x20000000u;
            break;
        }
        case 10: cmp_unsigned(cpu, rD >> 2, cpu->gpr[rA], uimm); break;            /* cmplwi */
        case 11: cmp_signed(cpu, rD >> 2, (s32)cpu->gpr[rA], simm); break;         /* cmpwi */
        case 12:                                                                    /* addic */
        case 13: {                                                                  /* addic. */
            u32 a = cpu->gpr[rA], r = a + (u32)simm;
            if (r < a) cpu->xer |= 0x20000000u; else cpu->xer &= ~0x20000000u;
            cpu->gpr[rD] = r;
            if (op == 13) record(cpu, r);
            break;
        }
        case 14: cpu->gpr[rD] = ra0 + (u32)simm; break;                             /* addi / li */
        case 15: cpu->gpr[rD] = ra0 + (uimm << 16); break;                          /* addis / lis */
        case 16: {                                                                  /* bc */
            u32 bo = rD, bi = rA;
            s32 bd = (s16)(raw & 0xFFFCu);
            if (cond_ok(cpu, bo, bi)) {
                next = (raw & 2u) ? (u32)bd : cia + (u32)bd;
                taken = true;
            }
            if (raw & 1u) cpu->lr = cia + 4u;
            break;
        }
        case 18: {                                                                  /* b / bl */
            s32 li = (s32)(raw << 6) >> 6;
            li &= ~3;
            next = (raw & 2u) ? (u32)li : cia + (u32)li;
            if (raw & 1u) cpu->lr = cia + 4u;
            taken = true;
            break;
        }
        case 19: {
            u32 xo = (raw >> 1) & 0x3FFu;
            if (xo == 16 || xo == 528) {                                            /* bclr / bcctr */
                u32 bo = rD, bi = rA;
                u32 target = xo == 16 ? (cpu->lr & ~3u) : (cpu->ctr & ~3u);
                bool ok = xo == 16 ? cond_ok(cpu, bo, bi)
                                   : ((bo & 16u) || (((cpu->cr >> (31u - bi)) & 1u) == ((bo >> 3) & 1u)));
                if (raw & 1u) cpu->lr = cia + 4u;
                if (ok) { next = target; taken = true; }
            } else if (xo == 150 || xo == 0) {                                      /* isync, mcrf (rare) */
                if (xo == 0) {
                    u32 src = (cpu->cr >> (28u - (rA >> 2) * 4u)) & 0xFu;
                    set_cr_field(cpu, rD >> 2, src);
                }
            } else if (xo == 193 || xo == 449 || xo == 33 || xo == 129 || xo == 257 || xo == 225 || xo == 289 ||
                       xo == 417) {                                                 /* CR logic */
                u32 a = (cpu->cr >> (31u - rA)) & 1u, b = (cpu->cr >> (31u - rB)) & 1u, r = 0;
                switch (xo) {
                case 193: r = a ^ b; break;          /* crxor */
                case 449: r = a | b; break;          /* cror */
                case 33:  r = !(a | b); break;       /* crnor */
                case 129: r = a & !b; break;         /* crandc */
                case 257: r = a & b; break;          /* crand */
                case 225: r = !(a & b); break;       /* crnand */
                case 289: r = !(a ^ b); break;       /* creqv */
                case 417: r = a | !b; break;         /* crorc */
                }
                cpu->cr = (cpu->cr & ~(1u << (31u - rD))) | (r << (31u - rD));
            } else {
                goto unknown;
            }
            break;
        }
        case 20: {                                                                  /* rlwimi */
            u32 sh = rB, mb = (raw >> 6) & 31u, me = (raw >> 1) & 31u, m = mask32(mb, me);
            u32 r = (rotl32(cpu->gpr[rD], sh) & m) | (cpu->gpr[rA] & ~m);
            cpu->gpr[rA] = r;
            if (raw & 1u) record(cpu, r);
            break;
        }
        case 21: {                                                                  /* rlwinm */
            u32 sh = rB, mb = (raw >> 6) & 31u, me = (raw >> 1) & 31u;
            u32 r = rotl32(cpu->gpr[rD], sh) & mask32(mb, me);
            cpu->gpr[rA] = r;
            if (raw & 1u) record(cpu, r);
            break;
        }
        case 24: cpu->gpr[rA] = cpu->gpr[rD] | uimm; break;                         /* ori */
        case 25: cpu->gpr[rA] = cpu->gpr[rD] | (uimm << 16); break;                 /* oris */
        case 26: cpu->gpr[rA] = cpu->gpr[rD] ^ uimm; break;                         /* xori */
        case 27: cpu->gpr[rA] = cpu->gpr[rD] ^ (uimm << 16); break;                 /* xoris */
        case 28: cpu->gpr[rA] = cpu->gpr[rD] & uimm; record(cpu, cpu->gpr[rA]); break;          /* andi. */
        case 29: cpu->gpr[rA] = cpu->gpr[rD] & (uimm << 16); record(cpu, cpu->gpr[rA]); break;  /* andis. */
        case 31: {
            u32 xo = (raw >> 1) & 0x3FFu;
            bool rc = raw & 1u;
            u32 a = cpu->gpr[rA], b = cpu->gpr[rB], s = cpu->gpr[rD];
            switch (xo) {
            case 0:   cmp_signed(cpu, rD >> 2, (s32)a, (s32)b); break;                     /* cmpw */
            case 32:  cmp_unsigned(cpu, rD >> 2, a, b); break;                             /* cmplw */
            case 266: cpu->gpr[rD] = a + b; if (rc) record(cpu, cpu->gpr[rD]); break;      /* add */
            case 40:  cpu->gpr[rD] = b - a; if (rc) record(cpu, cpu->gpr[rD]); break;      /* subf */
            case 104: cpu->gpr[rD] = (u32)-(s32)a; if (rc) record(cpu, cpu->gpr[rD]); break; /* neg */
            case 235: cpu->gpr[rD] = (u32)((s32)a * (s32)b); if (rc) record(cpu, cpu->gpr[rD]); break; /* mullw */
            case 444: cpu->gpr[rA] = s | b; if (rc) record(cpu, cpu->gpr[rA]); break;      /* or / mr */
            case 28:  cpu->gpr[rA] = s & b; if (rc) record(cpu, cpu->gpr[rA]); break;      /* and */
            case 60:  cpu->gpr[rA] = s & ~b; if (rc) record(cpu, cpu->gpr[rA]); break;     /* andc */
            case 316: cpu->gpr[rA] = s ^ b; if (rc) record(cpu, cpu->gpr[rA]); break;      /* xor */
            case 124: cpu->gpr[rA] = ~(s | b); if (rc) record(cpu, cpu->gpr[rA]); break;   /* nor */
            case 24:  cpu->gpr[rA] = (b & 32u) ? 0 : s << (b & 31u); if (rc) record(cpu, cpu->gpr[rA]); break; /* slw */
            case 536: cpu->gpr[rA] = (b & 32u) ? 0 : s >> (b & 31u); if (rc) record(cpu, cpu->gpr[rA]); break; /* srw */
            case 954: cpu->gpr[rA] = (u32)(s32)(s8)s; if (rc) record(cpu, cpu->gpr[rA]); break;  /* extsb */
            case 922: cpu->gpr[rA] = (u32)(s32)(s16)s; if (rc) record(cpu, cpu->gpr[rA]); break; /* extsh */
            case 23:  cpu->gpr[rD] = mem_read32(cpu, ra0 + b); break;                      /* lwzx */
            case 151: mem_write32(cpu, ra0 + b, s); break;                                 /* stwx */
            case 87:  cpu->gpr[rD] = mem_read8(cpu, ra0 + b); break;                       /* lbzx */
            case 215: mem_write8(cpu, ra0 + b, (u8)s); break;                              /* stbx */
            case 279: cpu->gpr[rD] = mem_read16(cpu, ra0 + b); break;                      /* lhzx */
            case 407: mem_write16(cpu, ra0 + b, (u16)s); break;                            /* sthx */
            case 339: {                                                                    /* mfspr */
                u32 spr = ((raw >> 16) & 31u) | (((raw >> 11) & 31u) << 5);
                if (spr == 8) cpu->gpr[rD] = cpu->lr;
                else if (spr == 9) cpu->gpr[rD] = cpu->ctr;
                else if (spr == 1) cpu->gpr[rD] = cpu->xer;
                else goto unknown;
                break;
            }
            case 467: {                                                                    /* mtspr */
                u32 spr = ((raw >> 16) & 31u) | (((raw >> 11) & 31u) << 5);
                if (spr == 8) cpu->lr = s;
                else if (spr == 9) cpu->ctr = s;
                else if (spr == 1) cpu->xer = s;
                else goto unknown;
                break;
            }
            case 19:  cpu->gpr[rD] = cpu->cr; break;                                       /* mfcr */
            case 144: {                                                                    /* mtcrf */
                u32 crm = (raw >> 12) & 0xFFu, m = 0;
                for (int i = 0; i < 8; i++) if (crm & (0x80u >> i)) m |= 0xF0000000u >> (i * 4);
                cpu->cr = (cpu->cr & ~m) | (s & m);
                break;
            }
            case 598: case 854: case 86: case 54: case 982: case 470: case 278: case 246:
                break;                                                                     /* sync, eieio, cache ops */
            default:
                goto unknown;
            }
            break;
        }
        case 32: cpu->gpr[rD] = mem_read32(cpu, ra0 + (u32)simm); break;                           /* lwz */
        case 33: { u32 ea = cpu->gpr[rA] + (u32)simm; cpu->gpr[rD] = mem_read32(cpu, ea); cpu->gpr[rA] = ea; break; } /* lwzu */
        case 34: cpu->gpr[rD] = mem_read8(cpu, ra0 + (u32)simm); break;                            /* lbz */
        case 36: mem_write32(cpu, ra0 + (u32)simm, cpu->gpr[rD]); break;                           /* stw */
        case 37: { u32 ea = cpu->gpr[rA] + (u32)simm; mem_write32(cpu, ea, cpu->gpr[rD]); cpu->gpr[rA] = ea; break; } /* stwu */
        case 38: mem_write8(cpu, ra0 + (u32)simm, (u8)cpu->gpr[rD]); break;                        /* stb */
        case 40: cpu->gpr[rD] = mem_read16(cpu, ra0 + (u32)simm); break;                           /* lhz */
        case 42: cpu->gpr[rD] = (u32)(s32)(s16)mem_read16(cpu, ra0 + (u32)simm); break;            /* lha */
        case 44: mem_write16(cpu, ra0 + (u32)simm, (u16)cpu->gpr[rD]); break;                      /* sth */
        case 46: {                                                                                  /* lmw */
            u32 ea = ra0 + (u32)simm;
            for (u32 r = rD; r < 32; r++, ea += 4) cpu->gpr[r] = mem_read32(cpu, ea);
            break;
        }
        case 47: {                                                                                  /* stmw */
            u32 ea = ra0 + (u32)simm;
            for (u32 r = rD; r < 32; r++, ea += 4) mem_write32(cpu, ea, cpu->gpr[r]);
            break;
        }
        case 48: cpu->fpr[rD] = f32_to_f64(mem_read32(cpu, ra0 + (u32)simm)); cpu->ps1[rD] = cpu->fpr[rD]; break; /* lfs */
        case 50: { u64 v = mem_read64(cpu, ra0 + (u32)simm); memcpy(&cpu->fpr[rD], &v, 8); break; }            /* lfd */
        case 52: mem_write32(cpu, ra0 + (u32)simm, f64_to_f32_bits(cpu->fpr[rD])); break;                      /* stfs */
        case 54: { u64 v; memcpy(&v, &cpu->fpr[rD], 8); mem_write64(cpu, ra0 + (u32)simm, v); break; }          /* stfd */
        default:
            goto unknown;
        }
        cpu->pc = next;
        if (taken)
            return INTERP_BRANCHED;
        continue;

    unknown:
        g_last_unknown = raw;
        g_unknown_pc = cia;
        cpu->pc = cia;
        fprintf(stderr, "interp: unsupported instruction 0x%08X at 0x%08X\n", raw, cia);
        return INTERP_UNSUPPORTED;
    }
    return INTERP_BUDGET;
}

void interp_report(void) {
    if (!g_runs)
        return;
    printf("run-time code (interpreted): %llu runs, %llu instructions, first at 0x%08X", g_runs, g_insns, g_first_pc);
    if (g_last_unknown)
        printf(", stopped at unsupported instruction 0x%08X at 0x%08X", g_last_unknown, g_unknown_pc);
    printf("\n");
}
