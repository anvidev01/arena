#include "heap.h"

#include "check_internal.h"
#include "os.h"

namespace arena {

// Static and zero-initialised: usable before main() and before any constructor runs (D-013).
constinit Span g_empty_span{};

namespace {

constexpr Heap make_initial_heap() {
  Heap h{};
  for (Span*& c : h.current) c = &g_empty_span;
  return h;
}

constinit Heap g_heap = make_initial_heap();

// --- span queue per size class --------------------------------------------------------

void refresh_current(Heap* h, std::size_t cls) {
  Span* first = h->queue[cls].first;
  h->current[cls] = first != nullptr ? first : &g_empty_span;
}

void queue_push_front(Heap* h, std::size_t cls, Span* s) {
  SpanQueue& q = h->queue[cls];
  s->prev = nullptr;
  s->next = q.first;
  if (q.first != nullptr) q.first->prev = s; else q.last = s;
  q.first = s;
  s->in_queue = true;
  refresh_current(h, cls);
}

// Spans that regain space go to the back, so the current span keeps serving (locality).
void queue_push_back(Heap* h, std::size_t cls, Span* s) {
  SpanQueue& q = h->queue[cls];
  s->next = nullptr;
  s->prev = q.last;
  if (q.last != nullptr) q.last->next = s; else q.first = s;
  q.last = s;
  s->in_queue = true;
  refresh_current(h, cls);
}

void queue_remove(Heap* h, std::size_t cls, Span* s) {
  SpanQueue& q = h->queue[cls];
  if (s->prev != nullptr) s->prev->next = s->next; else q.first = s->next;
  if (s->next != nullptr) s->next->prev = s->prev; else q.last = s->prev;
  s->next = s->prev = nullptr;
  s->in_queue = false;
  refresh_current(h, cls);
}

// --- pages and segments ---------------------------------------------------------------

Span* alloc_pages(Heap* h, std::size_t n) {
  for (Segment* seg = h->segments; seg != nullptr; seg = seg->next) {
    if (Span* s = segment_alloc_pages(seg, n)) return s;
  }
  Segment* seg = segment_create();
  if (seg == nullptr) return nullptr;
  ++h->stats.segments_mapped;
  seg->prev = nullptr;
  seg->next = h->segments;
  if (h->segments != nullptr) h->segments->prev = seg;
  h->segments = seg;
  return segment_alloc_pages(seg, n);
}

void release_pages(Heap* h, Span* s) {
  Segment* seg = segment_of(s);  // descriptors live in the header, so this works on `s` too
  segment_free_pages(seg, s);
  // Keep the last segment even when empty so a malloc/free loop doesn't mmap/munmap every
  // time. Smarter caching and decommit policies are Phase 4 experiments.
  const bool only_segment = h->segments == seg && seg->next == nullptr;
  if (!segment_is_empty(seg) || only_segment) return;
  if (seg->prev != nullptr) seg->prev->next = seg->next; else h->segments = seg->next;
  if (seg->next != nullptr) seg->next->prev = seg->prev;
  segment_destroy(seg);
  ++h->stats.segments_unmapped;
}

// --- small spans ----------------------------------------------------------------------

Span* new_small_span(Heap* h, std::size_t cls) {
  Span* s = alloc_pages(h, class_span_pages(cls));
  if (s == nullptr) return nullptr;
  s->kind = SpanKind::kSmall;
  s->size_class = static_cast<std::uint8_t>(cls);
  s->block_size = class_size(cls);
  s->capacity = static_cast<std::uint32_t>(s->page_count * kPageSize / s->block_size);
  s->used = 0;
  s->bump = s->start;  // nothing carved yet: no page of the span has been touched (D-011)
  s->free = s->local_free = nullptr;
  ++h->stats.spans_created;
  return s;
}

// Carve the next batch of never-used blocks into the free list. A batch covers about one
// OS page, so each extension touches (and makes resident) roughly one new page.
void extend_free_list(Span* s) {
  const std::size_t bs = s->block_size;
  const std::size_t remaining = static_cast<std::size_t>(span_end(s) - s->bump) / bs;
  std::size_t n = os::page_size() / bs;
  if (n == 0) n = 1;
  if (n > remaining) n = remaining;

  std::uint8_t* p = s->bump;
  for (std::size_t i = 0; i + 1 < n; ++i) {
    reinterpret_cast<Block*>(p + i * bs)->next = reinterpret_cast<Block*>(p + (i + 1) * bs);
  }
  reinterpret_cast<Block*>(p + (n - 1) * bs)->next = nullptr;
  s->free = reinterpret_cast<Block*>(p);
  s->bump = p + n * bs;
}

void retire_span(Heap* h, Span* s) {
  if (s->in_queue) queue_remove(h, s->size_class, s);
  release_pages(h, s);
  ++h->stats.spans_retired;
}

void free_small(Heap* h, Span* s, void* p) {
  ARENA_ASSERT(s->used > 0, "free(): double free or corrupted span (used == 0)");
  auto* b = static_cast<Block*>(p);
  b->next = s->local_free;
  s->local_free = b;
  --s->used;

  const std::size_t cls = s->size_class;
  if (!s->in_queue) {
    queue_push_back(h, cls, s);  // it was full; now it has a free block again
  } else if (s->used == 0 && h->current[cls] != s) {
    retire_span(h, s);  // completely empty and not the span malloc is using: give pages back
  }
}

}  // namespace

Heap* global_heap() { return &g_heap; }

void* heap_malloc_small_slow(Heap* h, std::size_t cls) {
  ++h->stats.slow_path;
  for (;;) {
    Span* s = h->current[cls];
    ARENA_ASSERT(s == &g_empty_span || (s->in_queue && s == h->queue[cls].first),
                 "malloc: current span is not the head of its queue");
    if (s == &g_empty_span) {
      s = new_small_span(h, cls);
      if (s == nullptr) return nullptr;
      queue_push_front(h, cls, s);
    }
    if (s->free == nullptr) {
      if (s->local_free != nullptr) {
        s->free = s->local_free;  // reuse recently freed (likely cached) blocks first
        s->local_free = nullptr;
      } else if (s->bump != span_end(s)) {
        extend_free_list(s);
      }
    }
    if (Block* b = s->free) {
      s->free = b->next;
      ++s->used;
      return b;
    }
    // Full. It leaves the queue until one of its blocks is freed; try the next span.
    queue_remove(h, cls, s);
  }
}

void* heap_malloc_large(Heap* h, std::size_t size) {
  const std::size_t n = (size + kPageSize - 1) >> kPageShift;
  Span* s = alloc_pages(h, n);
  if (s == nullptr) return nullptr;
  s->kind = SpanKind::kLarge;
  s->block_size = n * kPageSize;
  s->capacity = 1;
  s->used = 1;
  ++h->stats.large_allocs;
  return s->start;
}

void* heap_malloc_huge(Heap* h, std::size_t size) {
  Span* s = segment_create_huge(size);
  if (s == nullptr) return nullptr;
  ++h->stats.huge_allocs;
  return s->start;
}

void heap_free(Heap* h, void* p) {
  Span* s = span_of(p);
  // Cheap enough to keep in release: a page with no span means this pointer is not live.
  if (s == nullptr) os::fatal("arena: free(): invalid pointer or double free\n");

  switch (s->kind) {
    case SpanKind::kSmall:
      free_small(h, s, p);
      return;
    case SpanKind::kLarge:
      release_pages(h, s);
      ++h->stats.large_frees;
      return;
    case SpanKind::kHuge:
      ++h->stats.huge_frees;
      segment_destroy(segment_of(s));
      return;
    case SpanKind::kFree:
      break;
  }
  os::fatal("arena: free(): pointer into a free page\n");
}

std::size_t heap_usable_size(const void* p) {
  const Span* s = span_of(p);
  const auto* q = static_cast<const std::uint8_t*>(p);
  // Small pointers are always block starts (D-022); large/huge blocks begin at span start.
  const std::uint8_t* block = s->kind == SpanKind::kSmall ? q : s->start;
  return static_cast<std::size_t>(block + s->block_size - q);
}

void* heap_memalign(Heap* h, std::size_t alignment, std::size_t size) {
  // Precondition: alignment is a power of two (checked by the API layer).
  if (alignment <= kMinAlign) return heap_malloc(h, size);
  if (alignment > kMaxAlign) return nullptr;

  std::size_t n = size == 0 ? 1 : size;
  if (n > SIZE_MAX - alignment) return nullptr;
  n = (n + alignment - 1) & ~(alignment - 1);  // a multiple of the alignment

  if (alignment <= kPageSize) {
    // Spans start 64 KiB-aligned and block i sits at start + i * block_size, so a block is
    // aligned whenever its block size is a multiple of the alignment. Every power of two
    // from 16 B to 32 KiB is a class, so rounding n up to a power of two always works.
    // Large and huge blocks start 64 KiB-aligned already (D-022).
    if (n <= kMaxSmallSize && class_size(size_to_class(n)) % alignment != 0) {
      n = std::size_t{1} << (64 - __builtin_clzll(n - 1));
    }
    return heap_malloc(h, n);
  }

  // 64 KiB < alignment <= 2 MiB: over-allocate a large/huge block (start is 64 KiB-aligned)
  // and return an aligned pointer inside it. free() and usable_size() resolve interior
  // pointers of large/huge blocks through the page table, so nothing else is needed.
  const std::size_t extra = alignment - kPageSize;
  if (n > SIZE_MAX - extra) return nullptr;
  auto* p = static_cast<std::uint8_t*>(heap_malloc(h, n + extra));
  if (p == nullptr) return nullptr;
  const auto addr = reinterpret_cast<std::uintptr_t>(p);
  return reinterpret_cast<void*>((addr + alignment - 1) & ~(std::uintptr_t{alignment} - 1));
}

}  // namespace arena
