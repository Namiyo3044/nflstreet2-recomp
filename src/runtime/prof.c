#include "runtime/prof.h"
#include "runtime/symbols.h"
#include "gx/thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int g_prof_cat;

static CPUState* g_cpu;
static u32* g_pc_hist;                 /* samples per guest word, (pc - 0x80000000) / 4 */
static volatile unsigned g_now[PROF_COUNT];
static unsigned long long g_total[PROF_COUNT];
static unsigned g_taken[PROF_COUNT];
static bool g_started;

static const char* const k_names[PROF_COUNT] = {
    "game code", "gpu commands", "vertices", "textures", "locked cache/vmem",
    "audio mixing", "showing", "pacing sleep", "renderer wait",
};

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
