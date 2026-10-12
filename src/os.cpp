#include "os.h"

#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace arena::os {

namespace {

bool is_pow2(std::size_t x) { return x != 0 && (x & (x - 1)) == 0; }

void* map_anonymous(std::size_t size) {
  // MAP_NORESERVE: we reserve far more address space than we touch; don't let the
  // kernel's overcommit accounting count untouched pages against us.
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

}  // namespace

std::size_t page_size() {
  // Racing threads all store the same value, so relaxed ordering is enough; the atomic
  // only exists to make that benign race well-defined (and TSan-clean).
  static std::atomic<std::size_t> cached{0};
  std::size_t ps = cached.load(std::memory_order_relaxed);
  if (ps == 0) {
    ps = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    cached.store(ps, std::memory_order_relaxed);
  }
  return ps;
}

void* reserve_aligned(std::size_t size, std::size_t alignment) {
  const std::size_t ps = page_size();
  if (size == 0 || size % ps != 0 || !is_pow2(alignment) || alignment < ps) return nullptr;

  // Cheap attempt first: the kernel often hands out addresses that happen to be aligned.
  void* p = map_anonymous(size);
  if (p == nullptr) return nullptr;
  if ((reinterpret_cast<std::uintptr_t>(p) & (alignment - 1)) == 0) return p;
  release(p, size);

  // Over-map and trim (D-006). mmap results are page-aligned, so an aligned address is
  // at most (alignment - page) bytes past the start: size + alignment - page is enough.
  const std::size_t extra = alignment - ps;
  if (size > SIZE_MAX - extra) return nullptr;
  const std::size_t total = size + extra;
  p = map_anonymous(total);
  if (p == nullptr) return nullptr;

  const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(p);
  const std::uintptr_t aligned = (start + alignment - 1) & ~(std::uintptr_t{alignment} - 1);
  const std::size_t head = aligned - start;
  const std::size_t tail = total - head - size;
  if (head != 0) release(p, head);
  if (tail != 0) release(reinterpret_cast<void*>(aligned + size), tail);
  return reinterpret_cast<void*>(aligned);
}

void release(void* ptr, std::size_t size) { munmap(ptr, size); }

bool decommit(void* ptr, std::size_t size) {
  // On private anonymous memory, MADV_DONTNEED frees the physical pages immediately and
  // guarantees zero-filled pages on the next access (calloc will rely on this later).
  return madvise(ptr, size, MADV_DONTNEED) == 0;
}

void yield() { sched_yield(); }

void fatal(const char* msg) {
  ssize_t ignored = write(STDERR_FILENO, msg, std::strlen(msg));
  (void)ignored;
  std::abort();
}

}  // namespace arena::os
