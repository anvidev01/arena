// Layer L4: core API. Phase 1 has one global heap behind one spin lock (D-021). Phase 2
// replaces the lock with per-thread heaps.
#include <atomic>
#include <cstdint>
#include <cstring>

#include "api.h"
#include "arena/arena.h"
#include "heap.h"
#include "os.h"

namespace arena {

namespace {

// Test-and-test-and-set lock. Waiters spin on a plain load (which stays in their cache)
// and only retry the exchange once the lock looks free. acquire/release make the heap
// writes of one critical section visible to the next.
class SpinLock {
 public:
  void lock() {
    while (locked_.exchange(true, std::memory_order_acquire)) {
      while (locked_.load(std::memory_order_relaxed)) os::yield();
    }
  }
  void unlock() { locked_.store(false, std::memory_order_release); }

 private:
  std::atomic<bool> locked_{false};
};

constinit SpinLock g_lock;

struct Guard {
  Guard() { g_lock.lock(); }
  ~Guard() { g_lock.unlock(); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

bool is_pow2(std::size_t x) { return x != 0 && (x & (x - 1)) == 0; }

}  // namespace

void* malloc_impl(std::size_t size) {
  Guard g;
  return heap_malloc(global_heap(), size);
}

void free_impl(void* p) {
  if (p == nullptr) return;
  Guard g;
  heap_free(global_heap(), p);
}

void* calloc_impl(std::size_t count, std::size_t size) {
  std::size_t total;
  if (__builtin_mul_overflow(count, size, &total)) return nullptr;
  void* p = malloc_impl(total);
  // Zeroing happens outside the lock. Skipping it for fresh mmap memory is task 3.x.
  if (p != nullptr) std::memset(p, 0, total);
  return p;
}

std::size_t usable_size_impl(const void* p) {
  if (p == nullptr) return 0;
  Guard g;
  return heap_usable_size(p);
}

void* realloc_impl(void* p, std::size_t size) {
  if (p == nullptr) return malloc_impl(size);
  if (size == 0) {  // glibc behaviour: free and return NULL
    free_impl(p);
    return nullptr;
  }
  const std::size_t usable = usable_size_impl(p);
  // Stay in place if it fits and we'd waste at most half the block (tiny blocks always stay).
  if (size <= usable && (size >= usable / 2 || usable <= kLinearMax)) return p;

  void* q = malloc_impl(size);
  if (q == nullptr) return nullptr;  // C standard: the old block is left untouched
  std::memcpy(q, p, size < usable ? size : usable);
  free_impl(p);
  return q;
}

void* memalign_impl(std::size_t alignment, std::size_t size) {
  if (!is_pow2(alignment)) return nullptr;
  Guard g;
  return heap_memalign(global_heap(), alignment, size);
}

}  // namespace arena

extern "C" {

const char* arena_version(void) { return "0.1.0"; }
void* arena_malloc(size_t size) { return arena::malloc_impl(size); }
void arena_free(void* ptr) { arena::free_impl(ptr); }
void* arena_calloc(size_t count, size_t size) { return arena::calloc_impl(count, size); }
void* arena_realloc(void* ptr, size_t size) { return arena::realloc_impl(ptr, size); }
void* arena_memalign(size_t alignment, size_t size) { return arena::memalign_impl(alignment, size); }
size_t arena_usable_size(const void* ptr) { return arena::usable_size_impl(ptr); }

}  // extern "C"
