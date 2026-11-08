/* artful — the workers. A frame runs a few short loops one after the other, so what matters is how
 * soon a loop starts: workers spin a little after each one, which covers the gap to the next within
 * a frame, and sleep between frames. Work is taken index by index from one atomic counter. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* (sched_getaffinity) */
#endif
#include "core/threads.h"

#include "artful/core.h"

#include <stdatomic.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE Thread;
typedef CRITICAL_SECTION Mutex;
typedef CONDITION_VARIABLE Cond;
#define mutexInit(m) InitializeCriticalSection(m)
#define mutexFini(m) DeleteCriticalSection(m)
#define mutexLock(m) EnterCriticalSection(m)
#define mutexUnlock(m) LeaveCriticalSection(m)
#define condInit(c) InitializeConditionVariable(c)
#define condFini(c) ((void)0)
#define condWait(c, m) SleepConditionVariableCS(c, m, INFINITE)
#define condWakeAll(c) WakeAllConditionVariable(c)
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
typedef pthread_t Thread;
typedef pthread_mutex_t Mutex;
typedef pthread_cond_t Cond;
#define mutexInit(m) pthread_mutex_init(m, NULL)
#define mutexFini(m) pthread_mutex_destroy(m)
#define mutexLock(m) pthread_mutex_lock(m)
#define mutexUnlock(m) pthread_mutex_unlock(m)
#define condInit(c) pthread_cond_init(c, NULL)
#define condFini(c) pthread_cond_destroy(c)
#define condWait(c, m) pthread_cond_wait(c, m)
#define condWakeAll(c) pthread_cond_broadcast(c)
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#define spinPause() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define spinPause() __asm__ volatile("yield")
#else
#define spinPause() ((void)0)
#endif

#define MAX_THREADS 16
#define SPINS 20000 /* a few tens of microseconds: longer than the gaps inside a frame */

typedef struct Worker { ArtfPool* pool; uint32_t index; } Worker;

struct ArtfPool {
    int size;
    Thread threads[MAX_THREADS];
    Worker workers[MAX_THREADS];
    Mutex mutex;
    Cond wake;
    atomic_uint generation; /* a new loop to run */
    atomic_uint active;     /* the workers still in the loop */
    atomic_uint next;
    atomic_int sleeping;
    atomic_bool quit;
    ArtfJobFn fn;
    void* user;
    uint32_t count;
};

static void runLoop(ArtfPool* p, uint32_t worker) {
    for (;;) {
        uint32_t i = atomic_fetch_add_explicit(&p->next, 1, memory_order_relaxed);
        if (i >= p->count) return;
        p->fn(p->user, i, worker);
    }
}

static void workerMain(Worker* w) {
    ArtfPool* p = w->pool;
    unsigned seen = 0;
    for (;;) {
        int spins = 0;
        while (atomic_load_explicit(&p->generation, memory_order_acquire) == seen) {
            if (atomic_load_explicit(&p->quit, memory_order_relaxed)) return;
            if (++spins < SPINS) { spinPause(); continue; }
            mutexLock(&p->mutex);
            atomic_fetch_add(&p->sleeping, 1);
            while (atomic_load(&p->generation) == seen && !atomic_load(&p->quit)) condWait(&p->wake, &p->mutex);
            atomic_fetch_sub(&p->sleeping, 1);
            mutexUnlock(&p->mutex);
            spins = 0;
        }
        ++seen; /* (a loop waits for every worker: the count moves on by one at a time) */
        runLoop(p, w->index);
        atomic_fetch_sub_explicit(&p->active, 1, memory_order_release);
    }
}

#ifdef _WIN32
static DWORD WINAPI threadMain(void* w) { workerMain(w); return 0; }
static void threadStart(Thread* t, Worker* w) { *t = CreateThread(NULL, 0, threadMain, w, 0, NULL); }
static void threadJoin(Thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static int processorCount(void) {
    DWORD_PTR process = 0, system = 0;
    int n = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system))
        for (; process; process &= process - 1) ++n;
    return n;
}
double artfNowMs(void) {
    static double perMs = 0;
    LARGE_INTEGER t;
    if (perMs == 0) { QueryPerformanceFrequency(&t); perMs = (double)t.QuadPart / 1000.0; }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / perMs;
}
#else
static void* threadMain(void* w) { workerMain(w); return NULL; }
static void threadStart(Thread* t, Worker* w) { pthread_create(t, NULL, threadMain, w); }
static void threadJoin(Thread t) { pthread_join(t, NULL); }
static int processorCount(void) {
#ifdef __linux__
    /* what this process may use, which a container or a phone's scheduler may keep below what is there */
    cpu_set_t set;
    if (!sched_getaffinity(0, sizeof set, &set)) return CPU_COUNT(&set);
#endif
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}
double artfNowMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
#endif

ArtfPool* artfPoolNew(int threads) {
    ArtfPool* p = ARTF_NEW(ArtfPool);
    if (threads <= 0) threads = processorCount();
    p->size = artfClampi(threads, 1, MAX_THREADS);
    mutexInit(&p->mutex);
    condInit(&p->wake);
    for (int i = 1; i < p->size; ++i) {
        p->workers[i].pool = p;
        p->workers[i].index = (uint32_t)i;
        threadStart(&p->threads[i], &p->workers[i]);
    }
    return p;
}

void artfPoolFree(ArtfPool* p) {
    if (!p) return;
    mutexLock(&p->mutex);
    atomic_store(&p->quit, true);
    condWakeAll(&p->wake);
    mutexUnlock(&p->mutex);
    for (int i = 1; i < p->size; ++i) threadJoin(p->threads[i]);
    condFini(&p->wake);
    mutexFini(&p->mutex);
    artfFree(p);
}

int artfPoolSize(const ArtfPool* p) { return p->size; }

void artfPoolRun(ArtfPool* p, ArtfJobFn fn, void* user, uint32_t count) {
    if (p->size == 1) {
        for (uint32_t i = 0; i < count; ++i) fn(user, i, 0);
        return;
    }
    p->fn = fn;
    p->user = user;
    p->count = count;
    atomic_store_explicit(&p->next, 0, memory_order_relaxed);
    atomic_store_explicit(&p->active, (unsigned)(p->size - 1), memory_order_relaxed);
    atomic_fetch_add(&p->generation, 1); /* (seen before sleeping is read, as a worker going to sleep needs) */
    if (atomic_load(&p->sleeping) > 0) {
        mutexLock(&p->mutex);
        condWakeAll(&p->wake);
        mutexUnlock(&p->mutex);
    }
    runLoop(p, 0);
    while (atomic_load_explicit(&p->active, memory_order_acquire) != 0) spinPause();
}
