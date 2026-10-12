/*
 * Arena core API (layer L4, see docs/ARCHITECTURE.md).
 *
 * These are the allocator's own names. Tests, stress tests and benchmarks call them
 * directly, so they can run under ASan/TSan, which install their own malloc (D-003).
 * libarena.so additionally exports malloc/free/... that forward here (shim, task 1.8).
 */
#ifndef ARENA_ARENA_H
#define ARENA_ARENA_H

#include <stddef.h>

#define ARENA_API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

/* Version string, e.g. "0.0.1". */
ARENA_API const char* arena_version(void);

/* Same contracts as the C functions, except that they never set errno (the shim does).
 * malloc(0) returns a unique 16-byte block; realloc(p, 0) frees p and returns NULL (as glibc). */
ARENA_API void* arena_malloc(size_t size);
ARENA_API void  arena_free(void* ptr);
ARENA_API void* arena_calloc(size_t count, size_t size);
ARENA_API void* arena_realloc(void* ptr, size_t size);
ARENA_API size_t arena_usable_size(const void* ptr);

/* `alignment` must be a power of two, at most 2 MiB; otherwise returns NULL. */
ARENA_API void* arena_memalign(size_t alignment, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* ARENA_ARENA_H */
