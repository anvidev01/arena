// Layer L1 (task 1.3): page runs, and pointer -> segment -> page -> span.
#include "segment.h"

#include <cstdint>

#include "check.h"

using namespace arena;

TEST(new_segment_has_63_free_pages) {
  Segment* seg = segment_create();
  CHECK(seg != nullptr);
  if (!seg) return;
  CHECK(segment_is_empty(seg));
  CHECK_EQ(__builtin_popcountll(seg->free_pages), 63);
  CHECK_EQ(seg->free_pages & 1u, 0u);  // page 0 is the header
  segment_destroy(seg);
}

TEST(every_byte_of_a_span_maps_back_to_it) {
  Segment* seg = segment_create();
  CHECK(seg != nullptr);
  if (!seg) return;
  Span* one = segment_alloc_pages(seg, 1);
  Span* three = segment_alloc_pages(seg, 3);
  CHECK(one != nullptr && three != nullptr);
  if (!one || !three) return;

  CHECK(one->start == reinterpret_cast<std::uint8_t*>(seg) + kPageSize);  // first fit: page 1
  CHECK(three->start == one->start + kPageSize);
  CHECK_EQ(three->page_count, 3u);

  // Sample bytes across each span, including the first and last byte of every page.
  for (std::size_t off = 0; off < 3 * kPageSize; off += 4093) CHECK(span_of(three->start + off) == three);
  CHECK(span_of(three->start + 3 * kPageSize - 1) == three);
  CHECK(span_of(one->start) == one);
  CHECK(span_of(one->start + kPageSize - 1) == one);
  CHECK(segment_of(three->start + 12345) == seg);
  CHECK(segment_of(three) == seg);  // descriptors live inside the segment header
  CHECK(span_of(three->start + 3 * kPageSize) == nullptr);  // next page: not allocated

  std::uint8_t* three_start = three->start;  // the descriptor is wiped by free_pages
  segment_free_pages(seg, one);
  segment_free_pages(seg, three);
  CHECK(segment_is_empty(seg));
  CHECK(span_of(three_start) == nullptr);  // descriptors reset
  segment_destroy(seg);
}

TEST(first_fit_reuses_holes_and_reports_full) {
  Segment* seg = segment_create();
  CHECK(seg != nullptr);
  if (!seg) return;
  Span* a = segment_alloc_pages(seg, 10);
  Span* b = segment_alloc_pages(seg, 10);
  CHECK(a && b);
  if (!a || !b) return;
  std::uint8_t* a_start = a->start;
  segment_free_pages(seg, a);
  Span* c = segment_alloc_pages(seg, 4);  // fits in a's hole
  CHECK(c != nullptr && c->start == a_start);

  CHECK(segment_alloc_pages(seg, 64) == nullptr);  // more than 63 usable pages
  CHECK(segment_alloc_pages(seg, 0) == nullptr);
  // Free now: a 6-page hole (pages 5-10) and 43 pages after b (21-63).
  CHECK(segment_alloc_pages(seg, 43) != nullptr);
  CHECK(segment_alloc_pages(seg, 7) == nullptr);  // 6-page hole only
  CHECK(segment_alloc_pages(seg, 6) != nullptr);
  CHECK_EQ(seg->free_pages, 0u);
  segment_destroy(seg);
}

TEST(huge_segment_maps_interior_pointers) {
  const std::size_t size = 9 * 1024 * 1024 + 123;
  Span* s = segment_create_huge(size);
  CHECK(s != nullptr);
  if (!s) return;
  CHECK(s->kind == SpanKind::kHuge);
  CHECK(s->block_size >= size);
  CHECK_EQ(reinterpret_cast<std::uintptr_t>(s->start) % kPageSize, 0u);
  CHECK(span_of(s->start) == s);
  CHECK(span_of(s->start + 2 * 1024 * 1024) == s);  // memalign-style interior pointer
  s->start[size - 1] = 1;                            // the whole block is mapped
  segment_destroy(segment_of(s));
}

int main() { return arena_test::run_all(); }
