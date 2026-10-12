// Size classes (D-008, D-019). Every small request is rounded up to one of 40 sizes:
//
//   classes 0..7   : 16, 32, ..., 128                    (16-byte steps)
//   classes 8..39  : 4 classes per power of two, e.g. 160, 192, 224, 256, 320, ..., 32768
//
// Above 128 B a class is at most 25% bigger than the previous one, which bounds the
// rounding waste. Requests above kMaxSmallSize are "large" or "huge" and do not use classes.
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

namespace arena {

inline constexpr std::size_t kNumClasses = 40;
inline constexpr std::size_t kMaxSmallSize = 32 * 1024;
inline constexpr std::size_t kLinearClasses = 8;  // 16..128
inline constexpr std::size_t kLinearMax = kLinearClasses * kMinAlign;  // 128
inline constexpr std::size_t kMinBlocksPerSpan = 8;

// Above kMaxSmallSize: "large" (whole pages in a normal segment) up to kMaxLargeSize, then
// "huge" (a dedicated segment).
inline constexpr std::size_t kMaxLargeSize = std::size_t{1} << 20;  // 1 MiB

// Largest alignment memalign supports. Over-aligned huge blocks must keep the returned
// pointer inside the first 4 MiB of their segment for segment_of() to work (D-022).
inline constexpr std::size_t kMaxAlign = kSegmentSize / 2;  // 2 MiB

// Block size of class `cls` (cls < kNumClasses).
constexpr std::size_t class_size(std::size_t cls) {
  if (cls < kLinearClasses) return (cls + 1) * kMinAlign;
  const std::size_t j = cls - kLinearClasses;
  const std::size_t base = kLinearMax << (j / 4);  // 128, 256, ..., 16384
  return base + (j % 4 + 1) * (base / 4);
}

// Smallest class whose block fits `size` (1 <= size <= kMaxSmallSize). Constant time.
constexpr std::size_t size_to_class(std::size_t size) {
  if (size <= kLinearMax) return (size - 1) >> 4;  // size 0 is handled by the caller
  // For size in (2^k, 2^(k+1)], s = size - 1 has its top bit at k. The two bits below the
  // top bit say which quarter of that power-of-two range the size falls in.
  const std::size_t s = size - 1;
  const unsigned top = 63u - static_cast<unsigned>(__builtin_clzll(s));
  const std::size_t quarter = (s >> (top - 2)) & 3;
  return kLinearClasses + (top - 7) * 4 + quarter;  // 2^7 = 128 = kLinearMax
}

// Number of 64 KiB arena pages per span for `cls`, so that a span holds at least
// kMinBlocksPerSpan blocks. 1 page up to 8 KiB blocks, up to 4 pages for 32 KiB.
constexpr std::size_t class_span_pages(std::size_t cls) {
  const std::size_t bytes = class_size(cls) * kMinBlocksPerSpan;
  return bytes <= kPageSize ? 1 : (bytes + kPageSize - 1) / kPageSize;
}

static_assert(class_size(0) == 16);
static_assert(class_size(kLinearClasses - 1) == kLinearMax);
static_assert(class_size(8) == 160);
static_assert(class_size(kNumClasses - 1) == kMaxSmallSize);
static_assert(size_to_class(kMaxSmallSize) == kNumClasses - 1);

}  // namespace arena
