/* artful — the workers: threads kept for the life of a renderer, that run one loop at a time. */
#ifndef ARTFUL_THREADS_H
#define ARTFUL_THREADS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ArtfPool ArtfPool;

/* Called once for each index, on any thread; worker is that thread's number, below the pool's size. */
typedef void (*ArtfJobFn)(void* user, uint32_t index, uint32_t worker);

/* threads: 0: as many as the process may use, at most 16. */
ArtfPool* artfPoolNew(int threads);
void artfPoolFree(ArtfPool* p);
int artfPoolSize(const ArtfPool* p);
/* Runs fn over 0..count-1 and returns when all are done; the calling thread takes part. */
void artfPoolRun(ArtfPool* p, ArtfJobFn fn, void* user, uint32_t count);

/* A task: fn(user) on a thread of its own, for what the caller must not wait for (Vulkan's pipelines). Done says
 * whether it has returned; join waits for that, and frees the task. */
typedef struct ArtfTask ArtfTask;
ArtfTask* artfTaskStart(void (*fn)(void* user), void* user);
bool artfTaskDone(ArtfTask* t);
void artfTaskJoin(ArtfTask* t);

/* The calling thread, as a number no other running thread has; never 0. */
uintptr_t artfThreadId(void);

#endif
