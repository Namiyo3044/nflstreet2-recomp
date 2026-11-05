#include "gx/front.h"
#include "gx/fifo.h"
#include "gx/thread.h"
#include "runtime/mmio.h"
#include "runtime/prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* (see front.h) */

int g_gx_front_on;
volatile int g_gx_front_events;

/* The pipe's bytes, from the game thread (head) to this thread (tail).
   Positions only grow; a byte lives at pos & RING_MASK. */
#define RING_SIZE (1u << 25)           /* 32 MB: several frames' worth */
#define RING_MASK (RING_SIZE - 1u)
#define CHUNK (64u * 1024u)            /* handed to the parser at a time, so the tail moves on */
#define SPIN 500                       /* checks for more before going to sleep (tens of us) */

static u8* g_ring;
static size_t g_head;                  /* written by the game thread */
static size_t g_tail;                  /* written by this thread */
static size_t g_tail_seen;             /* (game thread) the tail as last read */
static CPUState* g_cpu;

static gx_mutex g_mx;
static gx_cond g_cv_work, g_cv_idle;
static volatile int g_idle;            /* the thread is waiting for work */
static volatile int g_wake_sent;       /* (game thread) woken since it went idle */
static NFLS2_TLS int t_on_thread;

static gx_mutex g_lock;                /* handing frames to the renderer / showing them */

/* Events for the game thread, in stream order. */
#define EV_N 4096u
static struct { int type; u32 value; } g_ev[EV_N];
static size_t g_ev_head;               /* written by this thread */
static size_t g_ev_tail;               /* written by the game thread */

static struct {
    unsigned long long bytes, chunks, syncs, idle_syncs, wakes, full_waits, event_waits, events;
    double busy_s, sync_s;
} g_st;

static double now_s(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

bool gx_front_on_thread(void) { return t_on_thread != 0; }

static void wake(void) {
    gx_mutex_lock(&g_mx);
    gx_cond_broadcast(&g_cv_work);
    gx_mutex_unlock(&g_mx);
}

static void front_main(void* arg) {
    (void)arg;
    t_on_thread = 1;
    t_prof_off = 1;                    /* (the profiler's marks are the game thread's) */
    size_t t = g_tail;
    for (;;) {
        size_t h = LOAD(&g_head);
        if (h == t) {
            /* Nothing yet: look again for a moment (the game writes in
               bursts), then sleep until woken (or 2 ms, in case a wake was
               missed). */
            int spins = 0;
            while (h == t && spins < SPIN) {
                gx_cpu_relax();
                h = LOAD(&g_head);
                spins++;
            }
            if (h == t) {
                gx_mutex_lock(&g_mx);
                g_wake_sent = 0;
                g_idle = 1;
                gx_cond_broadcast(&g_cv_idle);
                while ((h = LOAD(&g_head)) == t)
                    gx_cond_wait_ms(&g_cv_work, &g_mx, 2);
                g_idle = 0;
                gx_mutex_unlock(&g_mx);
            }
        }
        double t0 = now_s();
        while (t != h) {
            size_t off = t & RING_MASK, n = h - t;
            if (off + n > RING_SIZE) n = RING_SIZE - off;
            if (n > CHUNK) n = CHUNK;
            gx_fifo_feed(g_cpu, g_ring + off, n);
            t += n;
            STORE(&g_tail, t);
            g_st.bytes += n;
            g_st.chunks++;
        }
        g_st.busy_s += now_s() - t0;
    }
}

bool gx_front_start(CPUState* cpu) {
    if (g_gx_front_on)
        return true;
    g_ring = (u8*)malloc(RING_SIZE);
    if (!g_ring)
        return false;
    g_cpu = cpu;
    gx_mutex_init(&g_mx);
    gx_mutex_init(&g_lock);
    gx_cond_init(&g_cv_work);
    gx_cond_init(&g_cv_idle);
    gx_thread th;
    if (!gx_thread_create(&th, front_main, NULL)) {
        free(g_ring);
        g_ring = NULL;
        return false;
    }
    g_gx_front_on = 1;
    printf("graphics thread: on (commands, vertices and textures off the game thread)\n");
    return true;
}

void gx_front_push(u32 value, u8 size) {
    size_t h = g_head;
    if (h + size - g_tail_seen > RING_SIZE) {
        g_tail_seen = LOAD(&g_tail);
        if (h + size - g_tail_seen > RING_SIZE) {
            /* (full: several frames behind; wait for room) */
            g_st.full_waits++;
            int prev = prof_enter(PROF_GX_SYNC);
            wake();
            while (h + size - (g_tail_seen = LOAD(&g_tail)) > RING_SIZE) {
                if (g_gx_front_events) gx_front_drain();   /* (it may be waiting for room for those) */
                gx_cpu_relax();
            }
            prof_leave(prev);
        }
    }
    u32 be = value << (8u * (4u - size));
    for (u32 i = 0; i < size; i++)
        g_ring[(h + i) & RING_MASK] = (u8)(be >> (24u - 8u * i));
    STORE(&g_head, h + size);
    if (__atomic_load_n(&g_idle, __ATOMIC_RELAXED) && !g_wake_sent) {
        g_wake_sent = 1;
        g_st.wakes++;
        wake();
    }
}

void gx_front_event(int type, u32 value) {
    size_t h = g_ev_head;
    if (h - LOAD(&g_ev_tail) >= EV_N) {
        g_st.event_waits++;
        while (h - LOAD(&g_ev_tail) >= EV_N)
            gx_cpu_relax();
    }
    g_ev[h % EV_N].type = type;
    g_ev[h % EV_N].value = value;
    STORE(&g_ev_head, h + 1);
    STORE(&g_gx_front_events, 1);
}

void gx_front_drain(void) {
    if (!g_gx_front_on)
        return;
    STORE(&g_gx_front_events, 0);
    size_t t = g_ev_tail, h = LOAD(&g_ev_head);
    for (; t != h; t++) {
        u32 v = g_ev[t % EV_N].value;
        switch (g_ev[t % EV_N].type) {
        case GXF_EV_FINISH: mmio_pe_finish(); break;
        case GXF_EV_TOKEN: mmio_pe_token((u16)v, false); break;
        case GXF_EV_TOKEN_INT: mmio_pe_token((u16)v, true); break;
        case GXF_EV_DISPLAY_COPY: mmio_pe_display_copy(); break;
        }
        g_st.events++;
    }
    STORE(&g_ev_tail, t);
}

void gx_front_sync(void) {
    if (!g_gx_front_on)
        return;
    size_t h = g_head;
    if (LOAD(&g_tail) != h || !g_idle) {
        int prev = prof_enter(PROF_GX_SYNC);
        double t0 = now_s();
        gx_mutex_lock(&g_mx);
        gx_cond_broadcast(&g_cv_work);
        while (!(g_idle && LOAD(&g_tail) == h)) {
            if (g_gx_front_events) gx_front_drain();   /* (it may be waiting for room for those) */
            gx_cond_wait_ms(&g_cv_idle, &g_mx, 1);
        }
        gx_mutex_unlock(&g_mx);
        g_st.sync_s += now_s() - t0;
        g_st.syncs++;
        prof_leave(prev);
    }
    gx_front_drain();
}

bool gx_front_idle_sync(void) {
    if (!g_gx_front_on)
        return false;
    if (LOAD(&g_tail) == g_head && g_idle && !g_gx_front_events)
        return false;
    g_st.idle_syncs++;
    gx_front_sync();
    return true;
}

void gx_front_lock(void) {
    if (g_gx_front_on) gx_mutex_lock(&g_lock);
}

void gx_front_unlock(void) {
    if (g_gx_front_on) gx_mutex_unlock(&g_lock);
}

double gx_front_take_busy(void) {
    static double last;
    double b = g_st.busy_s, d = b - last;
    last = b;
    return d;
}

void gx_front_report(void) {
    if (!g_gx_front_on)
        return;
    printf("graphics thread: %.1f MB in %llu pieces, busy %.1f s; the game thread waited for it %llu times "
           "(%llu when idle), %.1f s in all; %llu events, %llu wakes, %llu waits for room\n",
           g_st.bytes / 1048576.0, g_st.chunks, g_st.busy_s, g_st.syncs, g_st.idle_syncs, g_st.sync_s,
           g_st.events, g_st.wakes, g_st.full_waits + g_st.event_waits);
}
