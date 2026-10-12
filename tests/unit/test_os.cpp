// Layer L0 (task 1.1): aligned reservation, release and decommit.
#include "os.h"

#include <cstdint>
#include <cstring>

#include "check.h"
#include "config.h"

using namespace arena;

static bool aligned(const void* p, std::size_t a) {
  return (reinterpret_cast<std::uintptr_t>(p) & (a - 1)) == 0;
}

TEST(page_size_is_power_of_two) {
  const std::size_t ps = os::page_size();
  CHECK(ps >= 4096);
  CHECK((ps & (ps - 1)) == 0);
  CHECK(kPageSize % ps == 0);  // arena pages must be whole OS pages
}

TEST(segment_reservation_is_aligned_zeroed_and_writable) {
  // Many in a row: the over-map-and-trim path must be hit and give aligned results each time.
  constexpr int kCount = 32;
  unsigned char* segs[kCount];
  for (int i = 0; i < kCount; ++i) {
    segs[i] = static_cast<unsigned char*>(os::reserve_aligned(kSegmentSize, kSegmentSize));
    CHECK(segs[i] != nullptr);
    if (segs[i] == nullptr) return;
    CHECK(aligned(segs[i], kSegmentSize));
    CHECK_EQ(segs[i][0], 0);                 // fresh anonymous memory is zero
    CHECK_EQ(segs[i][kSegmentSize - 1], 0);
    segs[i][0] = 0xAB;                       // both ends are writable
    segs[i][kSegmentSize - 1] = 0xCD;
  }
  // Distinct, non-overlapping regions.
  for (int i = 0; i < kCount; ++i)
    for (int j = i + 1; j < kCount; ++j) {
      const auto a = reinterpret_cast<std::uintptr_t>(segs[i]);
      const auto b = reinterpret_cast<std::uintptr_t>(segs[j]);
      CHECK(a + kSegmentSize <= b || b + kSegmentSize <= a);
    }
  for (int i = 0; i < kCount; ++i) os::release(segs[i], kSegmentSize);
}

TEST(huge_size_with_segment_alignment) {
  // Huge allocations (> 1 MiB) get a dedicated segment of any page-multiple size.
  const std::size_t size = 3 * kSegmentSize + 5 * os::page_size();
  void* p = os::reserve_aligned(size, kSegmentSize);
  CHECK(p != nullptr);
  CHECK(aligned(p, kSegmentSize));
  if (p) {
    static_cast<char*>(p)[size - 1] = 1;
    os::release(p, size);
  }
}

TEST(decommit_gives_back_zero_pages) {
  auto* p = static_cast<unsigned char*>(os::reserve_aligned(kSegmentSize, kSegmentSize));
  CHECK(p != nullptr);
  if (!p) return;
  std::memset(p, 0x5A, kPageSize);
  CHECK_EQ(p[100], 0x5A);
  CHECK(os::decommit(p, kPageSize));
  CHECK_EQ(p[100], 0);  // still mapped, contents gone
  os::release(p, kSegmentSize);
}

TEST(rejects_bad_arguments) {
  CHECK(os::reserve_aligned(0, kSegmentSize) == nullptr);
  CHECK(os::reserve_aligned(kSegmentSize + 1, kSegmentSize) == nullptr);   // not page multiple
  CHECK(os::reserve_aligned(kSegmentSize, kSegmentSize + 4096) == nullptr); // not power of 2
  CHECK(os::reserve_aligned(kSegmentSize, 8) == nullptr);                  // below page size
  CHECK(os::reserve_aligned(SIZE_MAX - os::page_size() + 1, kSegmentSize) == nullptr);
}

int main() { return arena_test::run_all(); }
