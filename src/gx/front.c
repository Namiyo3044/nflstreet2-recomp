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

/* Things one thread writes and the other reads each sit on a cache line of
   their own: sharing a line with something the other side writes made every
   pipe write bounce the line between cores (on a Ryzen with two core
   complexes, a slow trip each time). */
#define LINE _Alignas(64)

int g_gx_front_on;
LINE volatile int g_gx_front_events;

/* The pipe's bytes, from the game thread (head) to this thread (tail).
   Positions only grow; a byte lives at pos & RING_MASK. */
#define RING_SIZE (1u << 25)           /* 32 MB: several frames' worth */
#define RING_MASK (RING_SIZE - 1u)
#define CHUNK (64u * 1024u)            /* handed to the parser at a time, so the tail moves on */
#define SPIN 500                       /* checks for more before going to sleep (tens of us) */
#define BATCH 8192u                    /* bytes worth starting on at once ... */
#define BATCH_SPIN 1000                /* ... or after this many checks (tens of us), or at once when the game thread waits */
#define PUBLISH 1024u                  /* the game thread makes its bytes visible this many at a time (and when it waits) */

static u8* g_ring;
static CPUState* g_cpu;

/* The game thread's own (no other thread touches these). */
static struct {
    LINE size_t head;                  /* where the next byte goes */
    size_t pub;                        /* the head as last made visible */
    size_t tail_seen;                  /* the tail as last read */
    unsigned long long wakes, full_waits, syncs, idle_syncs, events;
    double sync_s;
} P;

/* Shared, each on its own line. */
static struct { LINE volatile size_t v; } g_head;        /* written by the game thread (every PUBLISH bytes) */
static struct { LINE volatile size_t v; } g_tail;        /* written by this thread */
static struct { LINE volatile int v; } g_idle;           /* this thread is waiting for work */
static struct { LINE volatile int v; } g_wake_sent;      /* woken since it went idle */
static struct { LINE volatile int v; } g_sync_req;       /* the game thread is waiting: no batching */

static gx_mutex g_mx;
static gx_cond g_cv_work, g_cv_idle;
static NFLS2_TLS int t_on_thread;

static gx_mutex g_lock;                /* handing frames to the renderer / showing them */

/* Events for the game thread, in stream order. */
#define EV_N 4096u
static struct { int type; u32 value; } g_ev[EV_N];
static struct { LINE volatile size_t v; } g_ev_head;     /* written by this thread */
static struct { LINE volatile size_t v; } g_ev_tail;     /* written by the game thread */

/* This thread's own counters. */
static struct {
    LINE unsigned long long bytes, chunks, event_waits;
    double busy_s;
} W;

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
    size_t t = g_tail.v;
    for (;;) {
        size_t h = LOAD(&g_head.v);
        if (h == t) {
            /* Nothing yet: look again for a moment (the game writes in
               bursts), then sleep until woken (or 2 ms, in case a wake was
               missed). */
            int spins = 0;
            while (h == t && spins < SPIN) {
                gx_cpu_relax();
                h = LOAD(&g_head.v);
                spins++;
            }
            if (h == t) {
                gx_mutex_lock(&g_mx);
                g_wake_sent.v = 0;
                g_idle.v = 1;
                gx_cond_broadcast(&g_cv_idle);
                while ((h = LOAD(&g_head.v)) == t)
                    gx_cond_wait_ms(&g_cv_work, &g_mx, 2);
                g_idle.v = 0;
                gx_mutex_unlock(&g_mx);
            }
        }
        /* A few bytes at a time cost more to hand over than to decode: wait a
           moment for a batch (the game thread only needs the results when
           it waits, and then it says so). */
        if (h - t < BATCH && !g_sync_req.v) {
            int spins = 0;
            while (h - t < BATCH && spins < BATCH_SPIN && !g_sync_req.v) {
                gx_cpu_relax();
                h = LOAD(&g_head.v);
                spins++;
            }
        }
        double t0 = now_s();
        while (t != h) {
            size_t off = t & RING_MASK, n = h - t;
            if (off + n > RING_SIZE) n = RING_SIZE - off;
            if (n > CHUNK) n = CHUNK;
            gx_fifo_feed(g_cpu, g_ring + off, n);
            t += n;
            STORE(&g_tail.v, t);
            W.bytes += n;
            W.chunks++;
        }
        W.busy_s += now_s() - t0;
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

/* Makes what the game thread wrote visible to this thread (and wakes it). */
static void publish(void) {
    P.pub = P.head;
    STORE(&g_head.v, P.pub);
    if (__atomic_load_n(&g_idle.v, __ATOMIC_RELAXED) && !g_wake_sent.v) {
        g_wake_sent.v = 1;
        P.wakes++;
        wake();
    }
}

void gx_front_flush(void) {
    if (g_gx_front_on && P.head != P.pub)
        publish();
}

void gx_front_push(u32 value, u8 size) {
    size_t h = P.head;
    if (h + size - P.tail_seen > RING_SIZE) {
        P.tail_seen = LOAD(&g_tail.v);
        if (h + size - P.tail_seen > RING_SIZE) {
            /* (full: several frames behind; wait for room) */
            P.full_waits++;
            int prev = prof_enter(PROF_GX_SYNC);
            publish();
            wake();
            while (h + size - (P.tail_seen = LOAD(&g_tail.v)) > RING_SIZE) {
                if (g_gx_front_events) gx_front_drain();   /* (it may be waiting for room for those) */
                gx_cpu_relax();
            }
            prof_leave(prev);
        }
    }
    u32 be = value << (8u * (4u - size));
    for (u32 i = 0; i < size; i++)
        g_ring[(h + i) & RING_MASK] = (u8)(be >> (24u - 8u * i));
    P.head = h + size;
    if (P.head - P.pub >= PUBLISH)
        publish();
}

void gx_front_event(int type, u32 value) {
    size_t h = g_ev_head.v;
    if (h - LOAD(&g_ev_tail.v) >= EV_N) {
        W.event_waits++;
        while (h - LOAD(&g_ev_tail.v) >= EV_N)
            gx_cpu_relax();
    }
    g_ev[h % EV_N].type = type;
    g_ev[h % EV_N].value = value;
    STORE(&g_ev_head.v, h + 1);
    STORE(&g_gx_front_events, 1);
}

void gx_front_drain(void) {
    if (!g_gx_front_on)
        return;
    STORE(&g_gx_front_events, 0);
    size_t t = g_ev_tail.v, h = LOAD(&g_ev_head.v);
    for (; t != h; t++) {
        u32 v = g_ev[t % EV_N].value;
        switch (g_ev[t % EV_N].type) {
        case GXF_EV_FINISH: mmio_pe_finish(); break;
        case GXF_EV_TOKEN: mmio_pe_token((u16)v, false); break;
        case GXF_EV_TOKEN_INT: mmio_pe_token((u16)v, true); break;
        case GXF_EV_DISPLAY_COPY: mmio_pe_display_copy(); break;
        }
        P.events++;
    }
    STORE(&g_ev_tail.v, t);
}

void gx_front_sync(void) {
    if (!g_gx_front_on)
        return;
    publish();
    size_t h = P.head;
    if (LOAD(&g_tail.v) != h || !g_idle.v) {
        int prev = prof_enter(PROF_GX_SYNC);
        double t0 = now_s();
        STORE(&g_sync_req.v, 1);
        gx_mutex_lock(&g_mx);
        gx_cond_broadcast(&g_cv_work);
        while (!(g_idle.v && LOAD(&g_tail.v) == h)) {
            if (g_gx_front_events) gx_front_drain();   /* (it may be waiting for room for those) */
            gx_cond_wait_ms(&g_cv_idle, &g_mx, 1);
        }
        gx_mutex_unlock(&g_mx);
        STORE(&g_sync_req.v, 0);
        P.sync_s += now_s() - t0;
        P.syncs++;
        prof_leave(prev);
    }
    gx_front_drain();
}

bool gx_front_idle_sync(void) {
    if (!g_gx_front_on)
        return false;
    if (P.head == P.pub && LOAD(&g_tail.v) == P.head && g_idle.v && !g_gx_front_events)
        return false;
    P.idle_syncs++;
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
    double b = W.busy_s, d = b - last;
    last = b;
    return d;
}

void gx_front_report(void) {
    if (!g_gx_front_on)
        return;
    printf("graphics thread: %.1f MB in %llu pieces, busy %.1f s; the game thread waited for it %llu times "
           "(%llu when idle), %.1f s in all; %llu events, %llu wakes, %llu waits for room\n",
           W.bytes / 1048576.0, W.chunks, W.busy_s, P.syncs, P.idle_syncs, P.sync_s,
           P.events, P.wakes, P.full_waits + W.event_waits);
}
