// Layer L0: the only code in Arena that talks to the kernel.
#pragma once

#include <cstddef>

namespace arena::os {

// OS page size in bytes (4 KiB on most x86-64, 4 or 16 KiB on AArch64). Never hardcode it.
std::size_t page_size();

// Map `size` bytes of zeroed, read-write memory whose address is a multiple of `alignment`.
// `size` must be a multiple of page_size(); `alignment` a power of two >= page_size().
// Returns nullptr on bad arguments or when the kernel refuses (D-006).
void* reserve_aligned(std::size_t size, std::size_t alignment);

// Unmap a range previously returned by reserve_aligned (or part of one).
void release(void* ptr, std::size_t size);

// Give the physical pages back to the kernel but keep the address range mapped.
// The next touch faults in a fresh zero page. Lowers RSS without unmapping.
bool decommit(void* ptr, std::size_t size);

// Give up the CPU (sched_yield). Used by the Phase 1 global lock while it waits.
void yield();

// Write `msg` to stderr with write(2) (no stdio: it may allocate) and abort.
[[noreturn]] void fatal(const char* msg);

}  // namespace arena::os
