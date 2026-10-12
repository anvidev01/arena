#include "segment.h"

#include "os.h"

namespace arena {

namespace {

std::uint64_t run_mask(std::size_t n) {
  return n >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << n) - 1;
}

}  // namespace

Segment* segment_create() {
  void* mem = os::reserve_aligned(kSegmentSize, kSegmentSize);
  if (mem == nullptr) return nullptr;
  // Fresh mmap memory is zero, so every descriptor already reads as kFree with null links.
  auto* seg = static_cast<Segment*>(mem);
  seg->size = kSegmentSize;
  seg->kind = SegmentKind::kNormal;
  seg->free_pages = kAllPagesFree;
  return seg;
}

Span* segment_create_huge(std::size_t size) {
  const std::size_t ps = os::page_size();
  if (size > SIZE_MAX - kPageSize - ps) return nullptr;
  const std::size_t total = (kPageSize + size + ps - 1) & ~(ps - 1);
  void* mem = os::reserve_aligned(total, kSegmentSize);
  if (mem == nullptr) return nullptr;

  auto* seg = static_cast<Segment*>(mem);
  seg->size = total;
  seg->kind = SegmentKind::kHuge;
  Span* span = &seg->pages[0];
  // Every page descriptor points at the one span, so any pointer into the first 4 MiB of
  // the block (memalign hands out interior pointers, D-022) finds it.
  for (Span& d : seg->pages) d.head = span;
  span->kind = SpanKind::kHuge;
  span->start = static_cast<std::uint8_t*>(mem) + kPageSize;
  span->block_size = total - kPageSize;
  span->capacity = 1;
  span->used = 1;
  return span;
}

void segment_destroy(Segment* seg) { os::release(seg, seg->size); }

Span* segment_alloc_pages(Segment* seg, std::size_t n) {
  if (n == 0 || n > kUsablePages) return nullptr;
  const std::uint64_t run = run_mask(n);
  for (std::size_t first = 1; first + n <= kPagesPerSegment; ++first) {
    const std::uint64_t mask = run << first;
    if ((seg->free_pages & mask) != mask) continue;

    seg->free_pages &= ~mask;
    Span* span = &seg->pages[first];
    for (std::size_t i = 0; i < n; ++i) seg->pages[first + i].head = span;
    span->start = reinterpret_cast<std::uint8_t*>(seg) + first * kPageSize;
    span->page_count = static_cast<std::uint32_t>(n);
    return span;
  }
  return nullptr;
}

void segment_free_pages(Segment* seg, Span* span) {
  const std::size_t first = static_cast<std::size_t>(span - seg->pages);
  const std::size_t n = span->page_count;
  for (std::size_t i = 0; i < n; ++i) seg->pages[first + i] = Span{};  // back to kFree
  seg->free_pages |= run_mask(n) << first;
}

}  // namespace arena
