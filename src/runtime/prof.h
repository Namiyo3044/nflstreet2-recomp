#ifndef NFLS2_PROF_H
#define NFLS2_PROF_H

#include "cpu/cpu.h"
#include <stddef.h>

/* Sampling profiler for the game thread. The game thread marks what it is
   doing with a plain store (prof_enter / prof_leave, almost free); a
   background thread looks about once a millisecond and counts what it sees,
   and while the game's own code runs it also records the guest pc, so the
   report can say which game functions take the time. */
enum {
    PROF_GAME,          /* the game's recompiled code (and anything not marked) */
    PROF_GX_FIFO,       /* graphics command stream parsing */
    PROF_GX_VERTEX,     /* vertex decode, transform, lighting, triangle recording */
    PROF_GX_TEXTURE,    /* texture cache lookups, hashing and decoding */
    PROF_MEM,           /* locked cache and virtual memory window accesses */
    PROF_AUDIO,         /* AX mixing */
    PROF_PRESENT,       /* showing the picture */
    PROF_SLEEP,         /* waiting for real time (pacing) */
    PROF_RENDER_WAIT,   /* waiting for the drawing threads */
    PROF_FILES,         /* reading the disc's files */
    PROF_CARD,          /* writing the memory card file */
    PROF_GX_SYNC,       /* waiting for the graphics thread to catch up */
    PROF_COUNT
};

extern volatile int g_prof_cat;

/* Only the game thread marks what it is doing; other threads that run the
   same code (the graphics thread: commands, vertices, textures) set
   t_prof_off and leave the mark alone. */
#if defined(_MSC_VER) && !defined(__clang__)
#define NFLS2_TLS __declspec(thread)
#else
#define NFLS2_TLS _Thread_local
#endif
extern NFLS2_TLS int t_prof_off;

static inline int prof_enter(int cat) {
    if (t_prof_off) return -1;
    int prev = g_prof_cat;
    g_prof_cat = cat;
    return prev;
}

static inline void prof_leave(int prev) {
    if (prev >= 0) g_prof_cat = prev;
}

/* Starts the sampler (once). */
void prof_start(CPUState* cpu);

/* Samples per category since the last call; returns the total. */
unsigned prof_take(unsigned out[PROF_COUNT]);

const char* prof_name(int cat);

/* The last few seconds of samples, for a closer look at one moment: a mark
   now, then (later) what the samples since that mark show: the shares of
   the categories and the game functions seen most ("" if no samples). */
unsigned prof_mark(void);
void prof_since(unsigned mark, char* out, size_t cap);

/* Whole run: time per category and the game functions that took the most. */
void prof_report(void);

#endif
