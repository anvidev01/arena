// Internal entry points behind both the arena_* API and the malloc shim. They have hidden
// visibility, so calls from the shim inside libarena.so are direct calls, not PLT jumps that
// another library could interpose.
#pragma once

#include <cstddef>

namespace arena {

void* malloc_impl(std::size_t size);
void free_impl(void* p);
void* calloc_impl(std::size_t count, std::size_t size);
void* realloc_impl(void* p, std::size_t size);
void* memalign_impl(std::size_t alignment, std::size_t size);  // alignment: power of two
std::size_t usable_size_impl(const void* p);

}  // namespace arena
