// Layer L3: the heap. Owns segments and keeps, per size class, a queue of spans that still
// have free blocks. Phase 1 has exactly one heap, protected by the global lock in alloc.cpp
// (D-021); Phase 2 gives every thread its own heap.
#pragma once

#include <cstddef>
#include <cstdint>

#include "segment.h"
#include "size_class.h"
#include "span.h"

namespace arena {

// Doubly linked list of spans of one size class that are not full.
struct SpanQueue {
  Span* first;
  Span* last;
};

// Counters for tests now and for the Phase 3 stats report.
struct HeapStats {
  std::uint64_t segments_mapped, segments_unmapped;
  std::uint64_t spans_created, spans_retired;  // small spans
  std::uint64_t large_allocs, large_frees;
  std::uint64_t huge_allocs, huge_frees;
  std::uint64_t slow_path;                     // small mallocs that missed the fast path
};

struct Heap {
  // current[c] is queue[c].first, or &g_empty_span when the queue is empty. The sentinel's
  // free list is always null, so the fast path needs no null check: an empty class simply
  // falls into the slow path (D-013).
  Span* current[kNumClasses];
  SpanQueue queue[kNumClasses];
  Segment* segments;  // normal segments (huge ones are not listed)
  HeapStats stats;
};

extern Span g_empty_span;

// The Phase 1 process-wide heap. Callers must hold the global lock.
Heap* global_heap();

void* heap_malloc_small_slow(Heap* h, std::size_t cls);
void* heap_malloc_large(Heap* h, std::size_t size);
void* heap_malloc_huge(Heap* h, std::size_t size);
void heap_free(Heap* h, void* p);
void* heap_memalign(Heap* h, std::size_t alignment, std::size_t size);
std::size_t heap_usable_size(const void* p);

// malloc: the fast path is inline. No lock (beyond the caller's), no atomics, no syscalls.
inline void* heap_malloc(Heap* h, std::size_t size) {
  if (size <= kMaxSmallSize) [[likely]] {
    const std::size_t cls = size_to_class(size + (size == 0));  // malloc(0) -> 16-byte block
    Span* s = h->current[cls];
    if (Block* b = s->free) [[likely]] {
      s->free = b->next;
      ++s->used;
      return b;
    }
    return heap_malloc_small_slow(h, cls);
  }
  if (size <= kMaxLargeSize) return heap_malloc_large(h, size);
  return heap_malloc_huge(h, size);
}

}  // namespace arena
