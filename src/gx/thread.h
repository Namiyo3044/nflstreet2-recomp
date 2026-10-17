#ifndef NFLS2_GX_THREAD_H
#define NFLS2_GX_THREAD_H

/* Minimal threads, mutex and condition variable: Win32 on Windows, pthreads
   elsewhere. Header-only so it can be shared by the renderer and tests. */

#ifdef _WIN32
/* Keep windows.h small: no GDI/USER names (DrawState, Rectangle, ...) or
   min/max macros leaking into the files that include this. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include <windows.h>

typedef HANDLE gx_thread;
typedef CRITICAL_SECTION gx_mutex;
typedef CONDITION_VARIABLE gx_cond;

typedef struct { void (*fn)(void*); void* arg; } gx_thread_start_;

static DWORD WINAPI gx_thread_entry_(LPVOID p) {
    gx_thread_start_ s = *(gx_thread_start_*)p;
    HeapFree(GetProcessHeap(), 0, p);
    s.fn(s.arg);
    return 0;
}

static inline int gx_thread_create(gx_thread* t, void (*fn)(void*), void* arg) {
    gx_thread_start_* s = (gx_thread_start_*)HeapAlloc(GetProcessHeap(), 0, sizeof(*s));
    if (!s)
        return 0;
    s->fn = fn;
    s->arg = arg;
    *t = CreateThread(NULL, 0, gx_thread_entry_, s, 0, NULL);
    return *t != NULL;
}
static inline void gx_mutex_init(gx_mutex* m) { InitializeCriticalSection(m); }
static inline void gx_mutex_lock(gx_mutex* m) { EnterCriticalSection(m); }
static inline void gx_mutex_unlock(gx_mutex* m) { LeaveCriticalSection(m); }
static inline void gx_cond_init(gx_cond* c) { InitializeConditionVariable(c); }
static inline void gx_cond_wait(gx_cond* c, gx_mutex* m) { SleepConditionVariableCS(c, m, INFINITE); }
static inline void gx_cond_broadcast(gx_cond* c) { WakeAllConditionVariable(c); }
static inline int gx_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#else
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

typedef pthread_t gx_thread;
typedef pthread_mutex_t gx_mutex;
typedef pthread_cond_t gx_cond;

typedef struct { void (*fn)(void*); void* arg; } gx_thread_start_;

static void* gx_thread_entry_(void* p) {
    gx_thread_start_ s = *(gx_thread_start_*)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

static inline int gx_thread_create(gx_thread* t, void (*fn)(void*), void* arg) {
    gx_thread_start_* s = (gx_thread_start_*)malloc(sizeof(*s));
    if (!s)
        return 0;
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(t, NULL, gx_thread_entry_, s) != 0) {
        free(s);
        return 0;
    }
    pthread_detach(*t);
    return 1;
}
static inline void gx_mutex_init(gx_mutex* m) { pthread_mutex_init(m, NULL); }
static inline void gx_mutex_lock(gx_mutex* m) { pthread_mutex_lock(m); }
static inline void gx_mutex_unlock(gx_mutex* m) { pthread_mutex_unlock(m); }
static inline void gx_cond_init(gx_cond* c) { pthread_cond_init(c, NULL); }
static inline void gx_cond_wait(gx_cond* c, gx_mutex* m) { pthread_cond_wait(c, m); }
static inline void gx_cond_broadcast(gx_cond* c) { pthread_cond_broadcast(c); }
static inline int gx_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
#endif

#endif
