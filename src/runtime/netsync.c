#include "runtime/netsync.h"
#include "runtime/interrupts.h"
#include "cpu/cpu.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Recording: "NS2INPUT", version, start clock, then one record per answer:
   'C' port value (a "controller there?" check) or 'P' port and the 8-byte
   poll reply. */
#define MAGIC "NS2INPUT"
#define VERSION 1u

static FILE* g_rec;
static FILE* g_play;
static bool g_play_ended, g_play_off_step;
static unsigned long long g_answers, g_off_step_at;
static bool g_rtc_fixed;
static uint32_t g_rtc;

static void put32(FILE* f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static bool get32(FILE* f, uint32_t* v) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *v = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return true;
}

bool netsync_record(const char* path) {
    g_rec = fopen(path, "wb");
    if (!g_rec) {
        fprintf(stderr, "netsync: couldn't write %s\n", path);
        return false;
    }
    /* The clock the run starts with (--rtc, else the PC's), kept for the
       whole run and stored for playback. */
    if (!g_rtc_fixed) netsync_set_rtc((uint32_t)(time(NULL) - 946684800));
    fwrite(MAGIC, 1, 8, g_rec);
    put32(g_rec, VERSION);
    put32(g_rec, g_rtc);
    printf("netsync: recording inputs to %s (clock %u)\n", path, g_rtc);
    return true;
}

bool netsync_play(const char* path) {
    FILE* f = fopen(path, "rb");
    char magic[8];
    uint32_t ver, rtc;
    if (!f || fread(magic, 1, 8, f) != 8 || memcmp(magic, MAGIC, 8) != 0 || !get32(f, &ver) || ver != VERSION ||
        !get32(f, &rtc)) {
        fprintf(stderr, "netsync: %s isn't an input recording\n", path);
        if (f) fclose(f);
        return false;
    }
    g_play = f;
    g_rtc = rtc;
    g_rtc_fixed = true;
    printf("netsync: playing back %s (clock %u)\n", path, rtc);
    return true;
}

bool netsync_playing(void) { return g_play != NULL && !g_play_ended; }

void netsync_set_rtc(uint32_t seconds) {
    g_rtc = seconds;
    g_rtc_fixed = true;
}

uint32_t netsync_rtc(uint32_t pc_clock) {
    return g_rtc_fixed ? g_rtc : pc_clock;
}

static void off_step(const char* what) {
    if (!g_play_off_step) {
        g_play_off_step = true;
        g_off_step_at = g_answers;
        printf("netsync: playback out of step at answer %llu, game time %.3f s (%s)\n", g_answers,
               interrupts_game_seconds(), what);
    }
}

static void play_ended(void) {
    if (!g_play_ended) {
        g_play_ended = true;
        printf("netsync: input recording ended at answer %llu, game time %.3f s; live input from here\n",
               g_answers, interrupts_game_seconds());
    }
}

bool netsync_connected(int port, bool live) {
    g_answers++;
    if (g_play && !g_play_ended) {
        int k = fgetc(g_play), p = fgetc(g_play), v = fgetc(g_play);
        if (k == EOF || p == EOF || v == EOF) { play_ended(); return live; }
        if (k != 'C' || p != port) off_step("controller check where the recording has something else");
        return v != 0;
    }
    if (g_rec) {
        fputc('C', g_rec); fputc(port, g_rec); fputc(live ? 1 : 0, g_rec);
    }
    return live;
}

void netsync_poll(int port, uint8_t out[8]) {
    g_answers++;
    if (g_play && !g_play_ended) {
        int k = fgetc(g_play), p = fgetc(g_play);
        uint8_t r[8];
        if (k == EOF || p == EOF || fread(r, 1, 8, g_play) != 8) { play_ended(); return; }
        if (k != 'P' || p != port) off_step("controller poll where the recording has something else");
        memcpy(out, r, 8);
        return;
    }
    if (g_rec) {
        fputc('P', g_rec); fputc(port, g_rec); fwrite(out, 1, 8, g_rec);
    }
}

/* ------------------------------------------------------------ sync log --- */
#define CHUNK 0x10000u

static FILE* g_log;
static unsigned g_every = 60;

bool netsync_log_open(const char* path, unsigned every) {
    g_log = fopen(path, "w");
    if (!g_log) {
        fprintf(stderr, "netsync: couldn't write %s\n", path);
        return false;
    }
    if (every) g_every = every;
    fprintf(g_log, "# sync log: every %u video fields, a hash of each 64 KB of game memory (0x80000000 up)\n",
            g_every);
    return true;
}

static uint64_t hash_block(const uint8_t* p, uint32_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

void netsync_field(CPUState* cpu, unsigned long long field) {
    if (!g_log || field % g_every != 0 || !cpu->ram) return;
    uint32_t n = cpu->ram_size / CHUNK;
    uint64_t all = 0xCBF29CE484222325ull;
    static uint32_t h[1024];
    for (uint32_t i = 0; i < n && i < 1024; i++) {
        uint64_t x = hash_block(cpu->ram + (size_t)i * CHUNK, CHUNK);
        h[i] = (uint32_t)(x ^ (x >> 32));
        all = (all ^ x) * 0x100000001B3ull;
    }
    fprintf(g_log, "field %llu answers %llu all %016llx chunks", field, g_answers, (unsigned long long)all);
    for (uint32_t i = 0; i < n && i < 1024; i++) fprintf(g_log, " %08x", h[i]);
    fputc('\n', g_log);
    fflush(g_log);
}

void netsync_report(void) {
    if (g_rec) {
        fflush(g_rec);
        printf("netsync: recorded %llu controller answers\n", g_answers);
    }
    if (g_play)
        printf("netsync: played back %llu controller answers%s\n", g_answers,
               g_play_off_step ? " (went out of step)" : g_play_ended ? " (recording ended first)" : "");
}
