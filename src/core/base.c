/* artful — memory. */
#include "artful/core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static ArtfLogFn logFn;
static void* logUser;

void artfSetLog(ArtfLogFn fn, void* user) {
    logFn = fn;
    logUser = user;
}

void artfLog(const char* format, ...) {
    if (!logFn) return;
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    logFn(logUser, text);
}

static void* checked(void* p) {
    if (p) return p;
    fputs("artful: out of memory\n", stderr);
    abort();
}

/* With ARTFUL_COUNT_ALLOCATIONS, what is alive is counted: a test then knows that all was given back. */
#ifdef ARTFUL_COUNT_ALLOCATIONS
#include <stdatomic.h>
static atomic_long alive;
long artfAllocationsAlive(void) { return atomic_load(&alive); }
#define COUNT(n) atomic_fetch_add(&alive, n)
#else
#define COUNT(n) ((void)0)
#endif

void* artfMalloc(size_t size) { COUNT(1); return checked(malloc(size ? size : 1)); }
void* artfCalloc(size_t count, size_t size) { COUNT(1); return checked(calloc(count ? count : 1, size ? size : 1)); }
void* artfRealloc(void* p, size_t size) {
    if (!p) COUNT(1);
    return checked(realloc(p, size ? size : 1));
}
void artfFree(void* p) {
    if (p) COUNT(-1);
    free(p);
}

/* (By hand: aligned_alloc is not on Windows. The block's start is kept just before what is returned.) */
void* artfAlignedMalloc(size_t size, size_t align) {
    char* raw = artfMalloc(size + align + sizeof(void*));
    char* p = (char*)(((uintptr_t)raw + sizeof(void*) + align - 1) & ~(uintptr_t)(align - 1));
    ((void**)p)[-1] = raw;
    return p;
}
void artfAlignedFree(void* p) {
    if (p) artfFree(((void**)p)[-1]);
}
