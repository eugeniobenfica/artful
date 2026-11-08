/* artful — the workers: threads kept for the life of a renderer, that run one loop at a time. */
#ifndef ARTFUL_THREADS_H
#define ARTFUL_THREADS_H

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

#endif
