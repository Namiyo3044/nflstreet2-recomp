#include "runtime/prof.h"
#include "runtime/symbols.h"
#include "gx/thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int g_prof_cat;
NFLS2_TLS int t_prof_off;

static CPUState* g_cpu;
static u32* g_pc_hist;                 /* samples per guest word, (pc - 0x80000000) / 4 */
static volatile unsigned g_now[PROF_COUNT];
static unsigned long long g_total[PROF_COUNT];
static unsigned g_taken[PROF_COUNT];
static bool g_started;

static const char* const k_names[PROF_COUNT] = {
    "game code", "gpu commands", "vertices", "textures", "locked cache/vmem",
    "audio mixing", "showing", "pacing sleep", "renderer wait", "disc files", "memory card",
    "waiting for graphics thread",
};

#define RING 4096                      /* about 4 s of samples */
static struct { unsigned char cat; u32 pc; } g_ring[RING];
static volatile unsigned g_ring_n;

const char* prof_name(int cat) {
    return cat >= 0 && cat < PROF_COUNT ? k_names[cat] : "?";
}

static void sleep_ms(void) {
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
}

static void sampler(void* arg) {
    (void)arg;
    for (;;) {
        sleep_ms();
        int c = g_prof_cat;
        if (c < 0 || c >= PROF_COUNT)
            continue;
        g_now[c]++;
        g_total[c]++;
        unsigned r = g_ring_n;
        g_ring[r % RING].cat = (unsigned char)c;
        g_ring[r % RING].pc = g_cpu->pc;
        g_ring_n = r + 1;
        if (c == PROF_GAME && g_pc_hist) {
            u32 slot = (g_cpu->pc - 0x80000000u) >> 2;
            if (slot < (24u << 20) / 4u)
                g_pc_hist[slot]++;
        }
    }
}

void prof_start(CPUState* cpu) {
    if (g_started)
        return;
    g_started = true;
    g_cpu = cpu;
    g_pc_hist = (u32*)calloc((24u << 20) / 4u, sizeof(u32));
    gx_thread t;
    gx_thread_create(&t, sampler, NULL);
}

unsigned prof_mark(void) { return g_ring_n; }

void prof_since(unsigned mark, char* out, size_t cap) {
    out[0] = 0;
    unsigned now = g_ring_n, n = now - mark;
    if (!n || !g_started) return;
    if (n > RING - 64) { n = RING - 64; mark = now - n; }
    unsigned cats[PROF_COUNT] = {0};
    u32 fn[64]; unsigned fc[64]; int nf = 0;
    for (unsigned i = mark; i != now; i++) {
        int c = g_ring[i % RING].cat;
        if (c >= PROF_COUNT) continue;
        cats[c]++;
        if (c != PROF_GAME) continue;
        u32 pc = g_ring[i % RING].pc, f = symbols_function_start(pc);
        if (!f) f = pc;
        int k = 0;
        while (k < nf && fn[k] != f) k++;
        if (k == nf) { if (nf == 64) continue; fn[nf] = f; fc[nf] = 0; nf++; }
        fc[k]++;
    }
    size_t o = 0;
    for (int c = 0; c < PROF_COUNT && o < cap; c++)
        if (cats[c] * 10 >= n)
            o += (size_t)snprintf(out + o, cap - o, "%s%s %u%%", o ? ", " : "", k_names[c], cats[c] * 100 / n);
    for (int t = 0; t < 3 && o < cap; t++) {
        int best = -1;
        for (int k = 0; k < nf; k++) if (fc[k] && (best < 0 || fc[k] > fc[best])) best = k;
        if (best < 0 || fc[best] * 20 < n) break;
        o += (size_t)snprintf(out + o, cap - o, "%s%s %u%%", t ? ", " : "; game code in ", symbols_lookup(fn[best], NULL),
                              fc[best] * 100 / n);
        fc[best] = 0;
    }
    if (o >= cap) out[cap - 1] = 0;
}

unsigned prof_take(unsigned out[PROF_COUNT]) {
    unsigned total = 0;
    for (int i = 0; i < PROF_COUNT; i++) {
        unsigned v = g_now[i];
        out[i] = v - g_taken[i];
        g_taken[i] = v;
        total += out[i];
    }
    return total;
}

/* What kind of instruction a guest word is (the game code's time by kind:
   where a faster code generator would gain the most). */
enum {
    K_LOAD, K_STORE, K_MULTI, K_FPMEM, K_PSQ, K_PS, K_FPS, K_FPD, K_FCMP, K_CMP, K_ALU, K_ALU_RC, K_ROT,
    K_MULDIV, K_BRANCH, K_SYS, K_OTHER, K_COUNT
};
static const char* const k_kind_names[K_COUNT] = {
    "integer loads", "integer stores", "load/store multiple", "float loads/stores", "quantized loads/stores",
    "paired-single maths", "single float maths", "double float maths / moves", "float compares",
    "integer compares", "integer maths", "integer maths setting cr0 (.)", "rotates / masks",
    "multiply / divide", "branches", "system / cache / special registers", "other",
};

static int insn_kind(u32 w) {
    u32 op = w >> 26, xo = (w >> 1) & 0x3FFu;
    switch (op) {
    case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43: return K_LOAD;
    case 36: case 37: case 38: case 39: case 44: case 45: return K_STORE;
    case 46: case 47: return K_MULTI;
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: return K_FPMEM;
    case 56: case 57: case 60: case 61: return K_PSQ;
    case 4: return (xo & 0x3Fu) == 6 || (xo & 0x3Fu) == 7 || (xo & 0x3Fu) == 38 || (xo & 0x3Fu) == 39 ? K_PSQ : K_PS;
    case 59: return K_FPS;
    case 63: return xo == 0 || xo == 32 ? K_FCMP : K_FPD;
    case 10: case 11: return K_CMP;
    case 7: return K_MULDIV;
    case 12: case 13: case 14: case 15: case 24: case 25: case 26: case 27: case 8: return K_ALU;
    case 28: case 29: return K_ALU_RC;     /* andi. andis. */
    case 20: case 21: case 23: return (w & 1u) ? K_ALU_RC : K_ROT;
    case 16: case 18: case 19:
        return op == 19 && (xo == 16 || xo == 528) ? K_BRANCH : op == 19 ? (xo == 150 || xo == 50 ? K_SYS : K_OTHER)
                                                                          : K_BRANCH;
    case 17: return K_SYS;
    case 31:
        if (xo == 0 || xo == 32) return K_CMP;
        if (xo == 23 || xo == 55 || xo == 87 || xo == 119 || xo == 279 || xo == 311 || xo == 343 || xo == 375 ||
            xo == 790 || xo == 534 || xo == 20)
            return K_LOAD;
        if (xo == 151 || xo == 183 || xo == 215 || xo == 247 || xo == 407 || xo == 439 || xo == 918 || xo == 662 ||
            xo == 150)
            return K_STORE;
        if (xo == 535 || xo == 567 || xo == 599 || xo == 631 || xo == 663 || xo == 695 || xo == 727 || xo == 759 ||
            xo == 983)
            return K_FPMEM;
        if ((xo & 0x1FFu) == 235 || (xo & 0x1FFu) == 75 || (xo & 0x1FFu) == 11 || (xo & 0x1FFu) == 491 ||
            (xo & 0x1FFu) == 459)
            return K_MULDIV;
        if (xo == 339 || xo == 467 || xo == 371 || xo == 83 || xo == 146 || xo == 595 || xo == 210 || xo == 86 ||
            xo == 54 || xo == 470 || xo == 278 || xo == 1014 || xo == 598 || xo == 854 || xo == 982 || xo == 144 ||
            xo == 19)
            return K_SYS;
        return (w & 1u) ? K_ALU_RC : K_ALU;
    }
    return K_OTHER;
}

static void report_kinds(void) {
    if (!g_cpu || !g_cpu->ram)
        return;
    unsigned long long kinds[K_COUNT] = {0}, all = 0;
    u32 n = (24u << 20) / 4u;
    for (u32 i = 0; i < n; i++) {
        if (!g_pc_hist[i])
            continue;
        u32 off = i * 4u;
        if (off + 4u > g_cpu->ram_size)
            continue;
        const u8* p = g_cpu->ram + off;
        u32 w = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
        kinds[insn_kind(w)] += g_pc_hist[i];
        all += g_pc_hist[i];
    }
    if (!all)
        return;
    printf("game code by kind of instruction (share of game-code time):\n");
    for (int t = 0; t < K_COUNT; t++) {
        int best = -1;
        for (int k = 0; k < K_COUNT; k++)
            if (kinds[k] && (best < 0 || kinds[k] > kinds[best])) best = k;
        if (best < 0)
            break;
        printf("  %5.1f%%  %s\n", 100.0 * (double)kinds[best] / (double)all, k_kind_names[best]);
        kinds[best] = 0;
    }
}

void prof_report(void) {
    if (!g_started)
        return;
    unsigned long long all = 0;
    for (int i = 0; i < PROF_COUNT; i++) all += g_total[i];
    if (!all)
        return;
    printf("\ntime on the game thread (sampled, whole run):");
    for (int i = 0; i < PROF_COUNT; i++)
        if (g_total[i])
            printf(" %s %.1f%%,", k_names[i], 100.0 * (double)g_total[i] / (double)all);
    printf("\n");
    if (!g_pc_hist || !g_total[PROF_GAME])
        return;
    report_kinds();
    /* Fold samples into functions (symbol start), then list the top ones. */
    u32 n = (24u << 20) / 4u;
    for (u32 i = 0; i < n; i++) {
        if (!g_pc_hist[i])
            continue;
        u32 start = symbols_function_start(0x80000000u + i * 4u);
        if (start && start != 0x80000000u + i * 4u) {
            u32 s = (start - 0x80000000u) >> 2;
            if (s < n) { g_pc_hist[s] += g_pc_hist[i]; g_pc_hist[i] = 0; }
        }
    }
    printf("game code by function (share of game-code time, top 30):\n");
    for (int k = 0; k < 30; k++) {
        u32 best = 0, at = 0;
        for (u32 i = 0; i < n; i++)
            if (g_pc_hist[i] > best) { best = g_pc_hist[i]; at = i; }
        if (!best)
            break;
        u32 addr = 0x80000000u + at * 4u;
        printf("  %5.1f%%  0x%08X  %s\n", 100.0 * best / (double)g_total[PROF_GAME], addr, symbols_lookup(addr, NULL));
        g_pc_hist[at] = 0;
    }
}
