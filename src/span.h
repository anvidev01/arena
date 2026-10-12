// Layer L2: spans. A span is a run of 1+ contiguous 64 KiB pages in a segment.
#pragma once

#include <cstddef>
#include <cstdint>

namespace arena {

// A free block stores the free-list link in its own first bytes: an intrusive list, so free
// memory needs no extra metadata (D-007). Live blocks contain only user data.
struct Block {
  Block* next;
};

enum class SpanKind : std::uint8_t {
  kFree = 0,  // page not in use (zeroed memory reads as kFree)
  kSmall,     // many blocks of one size class
  kLarge,     // one block of whole pages: 32 KiB < size <= 1 MiB
  kHuge,      // one block in a dedicated segment: size > 1 MiB
};

// Span descriptor. It lives in the segment header's page table, never inside the pages it
// describes, so a buffer overflow in user data cannot corrupt allocator metadata.
// Phase 1: only the single global heap touches spans, always under the global lock (D-021).
//
// Invariant for kSmall spans:
//   capacity == used + len(free) + len(local_free) + (end - bump) / block_size
struct Span {
  // --- fast path: malloc pops `free`, free pushes `local_free` -------------------------
  Block* free;              // malloc pops from here
  Block* local_free;        // frees push here; moved to `free` when it runs dry (D-009)
  std::uint32_t used;       // blocks currently handed out
  std::uint32_t capacity;   // blocks that fit in the span (1 for large and huge)

  // --- layout -----------------------------------------------------------------------
  std::uint8_t* start;      // first byte of the span's memory (64 KiB aligned)
  std::uint8_t* bump;       // [bump, end) has never been handed out (lazy fill, D-011)
  std::size_t block_size;   // bytes per block; for large/huge, the whole usable size
  std::uint32_t page_count; // 64 KiB pages covered (0 for huge)
  std::uint8_t size_class;  // kSmall only
  SpanKind kind;
  bool in_queue;            // linked in the heap's queue for its class (false => full)

  // --- links ------------------------------------------------------------------------
  Span* head;  // set in EVERY page descriptor of the span: points at the span's first one
  Span* next;  // heap queue for this size class
  Span* prev;
};

inline std::uint8_t* span_end(const Span* s) { return s->start + s->capacity * s->block_size; }

}  // namespace arena
