// Compile-time layout constants shared by every layer.
#pragma once

#include <cstddef>
#include <cstdint>

namespace arena {

// Segments: the unit Arena gets from the OS. Aligned to their own size, so
// `ptr & ~kSegmentMask` finds the segment header of any pointer (D-005).
inline constexpr std::size_t kSegmentShift = 22;  // 4 MiB
inline constexpr std::size_t kSegmentSize = std::size_t{1} << kSegmentShift;
inline constexpr std::uintptr_t kSegmentMask = kSegmentSize - 1;

// Arena pages: a segment is cut into 64 KiB pages; a span is 1+ contiguous pages.
// Not the same thing as an OS page (4 or 16 KiB), which os::page_size() reports.
inline constexpr std::size_t kPageShift = 16;  // 64 KiB
inline constexpr std::size_t kPageSize = std::size_t{1} << kPageShift;
inline constexpr std::size_t kPagesPerSegment = kSegmentSize / kPageSize;  // 64

// Every block is a multiple of 16 B, so every pointer meets alignof(max_align_t) (D-007).
inline constexpr std::size_t kMinAlign = 16;

static_assert(kSegmentSize % kPageSize == 0);
static_assert(alignof(std::max_align_t) <= kMinAlign);

}  // namespace arena
