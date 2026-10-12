// Size classes (task 1.2): exhaustively checks every small size, 1 .. 32 KiB.
#include "size_class.h"

#include "check.h"

using namespace arena;

TEST(classes_are_aligned_and_strictly_increasing) {
  for (std::size_t c = 0; c < kNumClasses; ++c) {
    CHECK_EQ(class_size(c) % kMinAlign, 0u);
    if (c > 0) CHECK(class_size(c) > class_size(c - 1));
  }
}

TEST(every_size_maps_to_the_smallest_fitting_class) {
  for (std::size_t size = 1; size <= kMaxSmallSize; ++size) {
    const std::size_t c = size_to_class(size);
    CHECK(c < kNumClasses);
    if (c >= kNumClasses) return;
    CHECK(class_size(c) >= size);                // fits
    if (c > 0) CHECK(class_size(c - 1) < size);  // and no smaller class would
  }
}

TEST(rounding_waste_is_bounded) {
  for (std::size_t size = 1; size <= kMaxSmallSize; ++size) {
    const std::size_t waste = class_size(size_to_class(size)) - size;
    if (size <= kLinearMax) {
      CHECK(waste < kMinAlign);
    } else {
      CHECK(waste * 4 < class_size(size_to_class(size)));  // < 25% of the block
    }
  }
}

TEST(spans_hold_enough_blocks) {
  for (std::size_t c = 0; c < kNumClasses; ++c) {
    const std::size_t blocks = class_span_pages(c) * kPageSize / class_size(c);
    CHECK(blocks >= kMinBlocksPerSpan);
  }
  CHECK_EQ(class_span_pages(size_to_class(8192)), 1u);
  CHECK_EQ(class_span_pages(kNumClasses - 1), 4u);
}

int main() { return arena_test::run_all(); }
