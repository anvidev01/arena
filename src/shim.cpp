// Layer L5: the glibc allocator names, exported from libarena.so only (D-002, D-003).
// LD_PRELOAD puts these ahead of glibc's, so every allocation in the process (including
// those made inside libc, libstdc++ and ld.so after startup) comes here.
//
// The whole family must be replaced: if one function were missing, glibc's version would
// hand out glibc memory that our free() cannot recognise.
//
// <stdlib.h> declares these as noexcept in C++, so the definitions must be noexcept too.
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include "api.h"
#include "os.h"

#define ARENA_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

bool is_pow2(std::size_t x) { return x != 0 && (x & (x - 1)) == 0; }

void* enomem_if_null(void* p) {
  if (p == nullptr) errno = ENOMEM;
  return p;
}

}  // namespace

ARENA_EXPORT void* malloc(std::size_t size) noexcept {
  return enomem_if_null(arena::malloc_impl(size));
}

ARENA_EXPORT void free(void* p) noexcept { arena::free_impl(p); }

ARENA_EXPORT void* calloc(std::size_t count, std::size_t size) noexcept {
  return enomem_if_null(arena::calloc_impl(count, size));
}

ARENA_EXPORT void* realloc(void* p, std::size_t size) noexcept {
  void* q = arena::realloc_impl(p, size);
  if (q == nullptr && !(p != nullptr && size == 0)) errno = ENOMEM;  // realloc(p, 0) frees
  return q;
}

ARENA_EXPORT void* reallocarray(void* p, std::size_t count, std::size_t size) noexcept {
  std::size_t total;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  return realloc(p, total);
}

ARENA_EXPORT int posix_memalign(void** out, std::size_t alignment, std::size_t size) noexcept {
  // POSIX: alignment must be a power of two and a multiple of sizeof(void*); errno untouched.
  if (!is_pow2(alignment) || alignment % sizeof(void*) != 0) return EINVAL;
  void* p = arena::memalign_impl(alignment, size);
  if (p == nullptr) return ENOMEM;
  *out = p;
  return 0;
}

ARENA_EXPORT void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
  if (!is_pow2(alignment)) {
    errno = EINVAL;
    return nullptr;
  }
  return enomem_if_null(arena::memalign_impl(alignment, size));
}

ARENA_EXPORT void* memalign(std::size_t alignment, std::size_t size) noexcept {
  // glibc rounds a non-power-of-two alignment up to the next power of two.
  if (!is_pow2(alignment)) {
    if (alignment > (std::size_t{1} << 63)) {
      errno = EINVAL;
      return nullptr;
    }
    alignment = std::size_t{1} << (64 - __builtin_clzll(alignment));
  }
  return enomem_if_null(arena::memalign_impl(alignment, size));
}

ARENA_EXPORT void* valloc(std::size_t size) noexcept {
  return enomem_if_null(arena::memalign_impl(arena::os::page_size(), size));
}

ARENA_EXPORT void* pvalloc(std::size_t size) noexcept {
  const std::size_t ps = arena::os::page_size();
  if (size > SIZE_MAX - ps) {
    errno = ENOMEM;
    return nullptr;
  }
  const std::size_t rounded = size == 0 ? ps : (size + ps - 1) & ~(ps - 1);
  return enomem_if_null(arena::memalign_impl(ps, rounded));
}

ARENA_EXPORT std::size_t malloc_usable_size(void* p) noexcept {
  return arena::usable_size_impl(p);
}
