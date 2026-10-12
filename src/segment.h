// Layer L1: segments. 4 MiB of memory aligned to 4 MiB, cut into 64 pages of 64 KiB (D-005).
//
//   segment base ─┬─ page 0  : Segment header (this struct, incl. the page table)
//                 ├─ page 1  : usable
//                 ├─ ...
//                 └─ page 63 : usable
//
// A huge allocation gets a dedicated segment of any size: header in the first 64 KiB, the
// block right after it, still 4 MiB aligned so segment_of() works the same way.
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "span.h"

namespace arena {

enum class SegmentKind : std::uint8_t { kNormal = 0, kHuge };

struct Segment {
  Segment* next;  // owning heap's list of normal segments
  Segment* prev;
  std::size_t size;          // bytes mapped: kSegmentSize, or more for huge
  std::uint64_t free_pages;  // bit i set => page i is free (normal segments only)
  SegmentKind kind;
  Span pages[kPagesPerSegment];  // one descriptor per 64 KiB page: the "page table"
};

static_assert(kPagesPerSegment == 64, "free_pages is a 64-bit mask");
static_assert(sizeof(Segment) <= kPageSize, "header must fit in page 0");

// Page 0 holds the header, so it is never free.
inline constexpr std::uint64_t kAllPagesFree = ~std::uint64_t{0} << 1;
inline constexpr std::size_t kUsablePages = kPagesPerSegment - 1;  // 63

// --- pointer -> metadata: two arithmetic steps, no search (the core trick of D-005) ------

inline Segment* segment_of(const void* p) {
  return reinterpret_cast<Segment*>(reinterpret_cast<std::uintptr_t>(p) & ~kSegmentMask);
}

inline std::size_t page_index(const Segment* seg, const void* p) {
  return (reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(seg)) >> kPageShift;
}

// Span that owns `p`, or nullptr if p's page is not part of any live span.
inline Span* span_of(const void* p) {
  Segment* seg = segment_of(p);
  return seg->pages[page_index(seg, p)].head;
}

// --- operations (segment.cpp) ---------------------------------------------------------

// Map a new normal segment; all usable pages free. nullptr if the OS refuses.
Segment* segment_create();

// Map a dedicated segment holding one block of at least `size` bytes; returns its span.
Span* segment_create_huge(std::size_t size);

// Unmap a segment (normal or huge).
void segment_destroy(Segment* seg);

// First-fit run of `n` contiguous free pages (1 <= n <= 63). Returns the run's first
// descriptor with `start`, `page_count` and every page's `head` set; nullptr if no room.
Span* segment_alloc_pages(Segment* seg, std::size_t n);

// Return a span's pages to its segment and reset their descriptors to kFree.
void segment_free_pages(Segment* seg, Span* span);

inline bool segment_is_empty(const Segment* seg) { return seg->free_pages == kAllPagesFree; }

}  // namespace arena
