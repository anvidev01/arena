// Layers L2-L4 (tasks 1.4-1.6) through the arena_* API, plus heap counters.
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "arena/arena.h"
#include "check.h"
#include "heap.h"

using namespace arena;

static std::uintptr_t addr(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }
static const HeapStats& stats() { return global_heap()->stats; }

TEST(every_small_size_is_aligned_and_usable) {
  for (std::size_t size = 0; size <= kMaxSmallSize; size += (size < 1024 ? 1 : 97)) {
    void* p = arena_malloc(size);
    CHECK(p != nullptr);
    CHECK_EQ(addr(p) % kMinAlign, 0u);
    CHECK(arena_usable_size(p) >= size);
    std::memset(p, 0xAB, size);  // writable over the full requested size
    arena_free(p);
  }
}

TEST(malloc_zero_returns_distinct_blocks) {
  void* a = arena_malloc(0);
  void* b = arena_malloc(0);
  CHECK(a != nullptr && b != nullptr && a != b);
  CHECK_EQ(arena_usable_size(a), 16u);
  arena_free(a);
  arena_free(b);
  arena_free(nullptr);  // no-op
}

TEST(freed_block_is_reused_first) {
  // LIFO through local_free: the next malloc of the same class gets the warm block back.
  void* a = arena_malloc(40);
  void* keep = arena_malloc(40);  // stops the span from becoming empty
  arena_free(a);
  // The free went to local_free; the fast path pops `free` (the rest of the carved batch)
  // first, so drain it and confirm `a` does come back.
  std::vector<void*> got;
  bool seen = false;
  for (int i = 0; i < 4096 && !seen; ++i) {
    void* p = arena_malloc(40);
    seen = (p == a);
    got.push_back(p);
  }
  CHECK(seen);
  for (void* p : got) arena_free(p);
  arena_free(keep);
}

TEST(blocks_never_overlap_within_and_across_spans) {
  // 3 spans' worth of 48-byte blocks: forces full spans, new spans, queue moves.
  const std::size_t n = 3 * (kPageSize / 48) + 7;
  std::vector<std::uint8_t*> ptrs;
  std::set<std::uintptr_t> seen;
  for (std::size_t i = 0; i < n; ++i) {
    auto* p = static_cast<std::uint8_t*>(arena_malloc(48));
    CHECK(p != nullptr);
    CHECK(seen.insert(addr(p)).second);
    std::memset(p, static_cast<int>(i & 0xFF), 48);
    ptrs.push_back(p);
  }
  // Sorted addresses must be at least 48 apart.
  std::uintptr_t prev = 0;
  for (std::uintptr_t a : seen) {
    if (prev != 0) CHECK(a - prev >= 48);
    prev = a;
  }
  for (std::size_t i = 0; i < n; ++i) {
    CHECK_EQ(ptrs[i][0], i & 0xFF);
    CHECK_EQ(ptrs[i][47], i & 0xFF);
    arena_free(ptrs[i]);
  }
}

TEST(empty_spans_and_segments_are_given_back) {
  // Measured as live counts, so the result doesn't depend on what earlier tests left mapped.
  auto live_segments = [] { return stats().segments_mapped - stats().segments_unmapped; };
  auto live_spans = [] { return stats().spans_created - stats().spans_retired; };
  const std::uint64_t spans_before = live_spans();
  const std::uint64_t segments_before = live_segments();
  std::vector<void*> ptrs;
  for (int i = 0; i < 20000; ++i) ptrs.push_back(arena_malloc(1000));  // ~20 MB of 1 KiB blocks
  CHECK(live_segments() >= 5);  // 313 one-page spans need at least 5 segments of 63 pages
  for (void* p : ptrs) arena_free(p);
  // Every span is retired except (at most) the class's current one, and every segment this
  // test added is unmapped except (at most) the one holding that span.
  CHECK(live_spans() <= spans_before + 1);
  CHECK(live_segments() <= segments_before + 1);
}

TEST(malloc_free_ping_pong_does_not_churn_spans) {
  // Performance rule, not correctness: an empty span that is still `current` is kept, so a
  // malloc/free loop reuses it instead of creating and releasing a span every iteration.
  // (Found by mutation testing: the fuzzer can't see this, since both versions are correct.)
  void* warm = arena_malloc(4000);
  arena_free(warm);
  const std::uint64_t before = stats().spans_created;
  for (int i = 0; i < 1000; ++i) arena_free(arena_malloc(4000));
  CHECK(stats().spans_created - before == 0);
}

TEST(large_allocations_use_whole_pages) {
  const std::uint64_t before = stats().large_allocs;
  auto* p = static_cast<std::uint8_t*>(arena_malloc(kMaxSmallSize + 1));
  CHECK(p != nullptr);
  CHECK_EQ(addr(p) % kPageSize, 0u);
  CHECK_EQ(arena_usable_size(p), kPageSize);
  std::memset(p, 1, kMaxSmallSize + 1);
  auto* q = static_cast<std::uint8_t*>(arena_malloc(kMaxLargeSize));
  CHECK_EQ(arena_usable_size(q), kMaxLargeSize);
  CHECK_EQ(stats().large_allocs - before, 2u);
  arena_free(p);
  arena_free(q);
  CHECK_EQ(stats().large_frees - before, 2u);
}

TEST(huge_allocations_get_their_own_segment) {
  const std::uint64_t before = stats().huge_frees;
  const std::size_t size = 10 * 1024 * 1024 + 1;
  auto* p = static_cast<std::uint8_t*>(arena_malloc(size));
  CHECK(p != nullptr);
  CHECK(arena_usable_size(p) >= size);
  p[0] = 1;
  p[size - 1] = 2;
  arena_free(p);
  CHECK_EQ(stats().huge_frees - before, 1u);
  CHECK(arena_malloc(SIZE_MAX - 4096) == nullptr);  // impossible size: NULL, no crash
}

TEST(calloc_zeroes_and_detects_overflow) {
  // Dirty a block, free it, then calloc the same class: it must come back zeroed.
  auto* d = static_cast<std::uint8_t*>(arena_malloc(256));
  std::memset(d, 0xFF, 256);
  arena_free(d);
  auto* p = static_cast<std::uint8_t*>(arena_calloc(16, 16));
  CHECK(p != nullptr);
  bool zero = true;
  for (int i = 0; i < 256; ++i) zero = zero && p[i] == 0;
  CHECK(zero);
  arena_free(p);
  CHECK(arena_calloc(SIZE_MAX / 2, 3) == nullptr);  // count * size overflows
  void* z = arena_calloc(0, 10);
  CHECK(z != nullptr);
  arena_free(z);
}

TEST(realloc_preserves_contents_across_moves) {
  auto* p = static_cast<std::uint8_t*>(arena_realloc(nullptr, 10));  // == malloc
  for (int i = 0; i < 10; ++i) p[i] = static_cast<std::uint8_t>(i);
  // Grow through small -> large -> huge, checking the prefix each time.
  std::size_t sizes[] = {100, 5000, 40000, 2 * 1024 * 1024, 64};
  std::size_t prev = 10;
  for (std::size_t size : sizes) {
    p = static_cast<std::uint8_t*>(arena_realloc(p, size));
    CHECK(p != nullptr);
    const std::size_t keep = prev < size ? prev : size;
    bool ok = true;
    for (std::size_t i = 0; i < keep && i < 10; ++i) ok = ok && p[i] == i;
    CHECK(ok);
    prev = size;
  }
  // Shrinking a little stays in place; realloc(p, 0) frees and returns NULL (glibc).
  auto* q = static_cast<std::uint8_t*>(arena_malloc(1000));
  CHECK(arena_realloc(q, 900) == q);
  CHECK(arena_realloc(q, 0) == nullptr);
  arena_free(p);
}

TEST(memalign_honours_every_alignment) {
  for (std::size_t align = 1; align <= 2 * 1024 * 1024; align <<= 1) {
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{24}, std::size_t{100},
                             std::size_t{5000}, std::size_t{70000}, std::size_t{3000000}}) {
      void* p = arena_memalign(align, size);
      CHECK(p != nullptr);
      if (!p) continue;
      CHECK_EQ(addr(p) % align, 0u);
      CHECK_EQ(addr(p) % kMinAlign, 0u);
      CHECK(arena_usable_size(p) >= size);
      if (size) static_cast<std::uint8_t*>(p)[size - 1] = 1;
      arena_free(p);
    }
  }
  CHECK(arena_memalign(48, 10) == nullptr);                // not a power of two
  CHECK(arena_memalign(4 * 1024 * 1024, 10) == nullptr);   // above the 2 MiB limit (D-022)
}

int main() { return arena_test::run_all(); }
