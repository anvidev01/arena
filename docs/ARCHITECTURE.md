# Arena — Architecture

**Arena** is a thread-caching `malloc` replacement for Linux. It loads into any unmodified program with `LD_PRELOAD`, and it is benchmarked against glibc, jemalloc and mimalloc.

This document describes *what* the allocator is made of and *how a pointer moves through it*. The reasons behind each choice are recorded as numbered decisions (D-xxx) in [NOTEBOOK.md](NOTEBOOK.md). Each D-number below links to its full reasoning there.

> Status: **Phase 1 complete (2026-10-09).** L0–L5 work for a single heap guarded by one global lock (D-021). Per-thread heaps, `thread_free` and abandonment (marked *Phase 2* below) are still design only. Numbers such as segment size and class count are starting values that Phase 4 benchmarks may change. When one changes, update this file and add a NOTEBOOK entry.

---

## 1. Goals and non-goals

**Goals**
1. **Fast path with no locks and no atomic instructions.** A `malloc` or `free` by the thread that owns the memory should be a handful of loads, stores and branches.
2. **Cross-thread frees are correct and cheap.** Memory freed by thread B but allocated by thread A goes back to A through a lock-free list.
3. **Drop-in.** Programs such as `ls`, `python3`, `gcc` and Redis run unmodified under `LD_PRELOAD=libarena.so`.
4. **Measurable.** Built-in statistics, plus a benchmark harness for throughput, p50/p99/p99.9 latency, RSS and thread scaling.
5. **Explainable.** Every design choice has a reason you can say in an interview.

**Non-goals (for the 4-week scope)**
- macOS or Windows support. macOS uses malloc zones, not `LD_PRELOAD` (D-002).
- NUMA awareness and security hardening beyond the debug mode.
- Beating mimalloc everywhere. The goal is to *explain* every win and loss.

---

## 2. The big picture

```
 application code
      │  malloc / free / calloc / realloc / posix_memalign / ...
      ▼
┌─────────────────────────────────────────────────────────────┐
│ L5  Shim            src/shim.cpp      exported C symbols    │  only in libarena.so
├─────────────────────────────────────────────────────────────┤
│ L4  Core API        arena_malloc / arena_free / ...         │  libarena_core.a (tests link this)
├─────────────────────────────────────────────────────────────┤
│ L3  Thread heap     per-thread: span queue per size class   │  no locks, no atomics
├─────────────────────────────────────────────────────────────┤
│ L2  Span            64 KiB page(s) carved into equal blocks │  3 free lists (1 atomic)
├─────────────────────────────────────────────────────────────┤
│ L1  Segment         4 MiB, 4 MiB-aligned region from mmap   │  owned by one heap
├─────────────────────────────────────────────────────────────┤
│ L0  OS              mmap / munmap / madvise / huge pages    │  src/os.cpp
└─────────────────────────────────────────────────────────────┘
```

Each layer talks only to the layer directly below it. That keeps each one testable on its own: L0–L4 are unit-tested without ever replacing the system `malloc`.

---

## 3. Layer by layer

### L0 — OS layer (`src/os.cpp`)
- `os::reserve_aligned(size, align)` returns memory whose address is a multiple of `align`. `mmap` only guarantees page alignment, so it first tries a plain `mmap` (often already aligned). If that fails, it maps `size + align − page_size` and `munmap`s the unaligned head and tail (D-006).
- `os::decommit(ptr, size)` calls `madvise(MADV_DONTNEED)`. This returns physical pages to the kernel but keeps the address range reserved, which lowers RSS. (Not used by the heap yet; that is a Phase 4 policy.)
- `os::fatal(msg)` writes with `write(2)` and aborts; `os::yield()` wraps `sched_yield` for the Phase 1 lock.
- `os::page_size()` reads `sysconf(_SC_PAGESIZE)` once. Arena never hardcodes 4096, because some ARM Linux machines use 16 KiB pages.
- Optional huge pages through `madvise(MADV_HUGEPAGE)` on segments (Phase 4 experiment).
- **Rule:** this layer is the *only* place Arena calls the kernel.

### L1 — Segment (`src/segment.cpp`)
A **segment** is a 4 MiB block of memory aligned to 4 MiB (D-005).

```
segment base (4 MiB aligned)
├── page 0  : [ Segment header: links, size, free-page bitmap, page table of 64 Span descriptors ]
├── page 1  : 64 KiB   ┐
├── ...                ├ 63 usable pages, handed out first-fit as runs of 1–63 pages
└── page 63 : 64 KiB   ┘
```
The header is about 6 KiB, but page 0 is reserved for it entirely (D-023). The rest of page 0 is never touched, so it costs address space, not RAM.

- Because segments are aligned, **finding the metadata for any pointer is two arithmetic operations, with no search and no lookup table:**
  ```
  segment = ptr & ~(4 MiB - 1)              // clear the low 22 bits
  page    = (ptr - segment) >> 16           // which 64 KiB page
  span    = segment->pages[page].head       // metadata for that page (src/segment.h: span_of)
  ```
  Every page descriptor of a multi-page span has `head` pointing at the span's first descriptor, so a pointer into *any* page of the span finds the same metadata. A page that belongs to no span has `head == nullptr`; `free` of such a pointer aborts with "invalid pointer or double free" (D-027).
- A 64-bit bitmap `free_pages` records which pages are free; finding a run of `n` free pages is a scan over at most 63 masks (slow path only).
  This is the core trick that makes `free(ptr)` cheap without a header before every block.
- Each segment belongs to exactly one heap (in Phase 1, the single global heap). Getting a new segment from the OS is the only slow step, and it happens about once per 4 MiB.
- **Huge allocations** (more than 1 MiB) get their own dedicated segment, sized to fit and still 4 MiB-aligned, so the same pointer-to-segment math works for them too. The header sits in the first 64 KiB, the block starts right after it, and all 64 page descriptors point at the one huge span.
- **Empty segments** are unmapped as soon as they become empty, except the heap's last segment, which is kept so a malloc/free loop doesn't `mmap`/`munmap` every time (D-024).

### L2 — Span (`src/span.h`)
A **span** is one or more contiguous 64 KiB pages. A *small* span holds equal blocks of one size class; a *large* span is one block of whole pages (32 KiB – 1 MiB); a *huge* span is the single block of a dedicated segment.

```
struct Span {                  // lives in the segment's page table, not inside the pages
  Block*   free;               // malloc pops here                      (fast path)
  Block*   local_free;         // free pushes here                      (fast path)
  uint32_t used, capacity;     // live blocks / blocks that fit
  uint8_t* start;              // first byte (64 KiB aligned)
  uint8_t* bump;               // [bump, end) never handed out (lazy fill)
  size_t   block_size;         // per block; whole usable size for large/huge
  uint32_t page_count;
  uint8_t  size_class;  SpanKind kind;  bool in_queue;
  Span*    head;               // in every page descriptor: the span's first descriptor
  Span*    next, *prev;        // heap queue for this class
  // Phase 2 adds: std::atomic<Block*> thread_free;  Heap* owner;
};
// Invariant (small): capacity == used + len(free) + len(local_free) + (end - bump) / block_size
```

**Three free lists: why not one?** (D-009, the mimalloc "free list sharding" idea)
- `free` — `malloc` pops from it. Only the owning thread touches it, so it needs no atomics.
- `local_free` — owner-thread `free`s push here, *not* onto `free`. This makes the fast path a plain pop until `free` runs out, and gives one natural point to do bookkeeping: the moment `free` empties.
- `thread_free` — `free`s from *other* threads push here with a compare-and-swap (CAS). The owner later takes the **whole list at once** with `exchange(nullptr)`. Because the consumer never pops one node at a time, the classic **ABA problem cannot happen** (D-010).

The free list is **intrusive**: a free block stores the `next` pointer in its own first 8 bytes. Free memory therefore costs no extra metadata, and that is why the minimum block size is 16 bytes (D-007).

New spans are filled **lazily**. Arena does not thread all blocks into a list up front, which would touch every page and inflate RSS. Instead it keeps a "bump" pointer into untouched memory. Each time the lists run dry it carves the next batch, about one OS page worth of blocks, so each extension makes roughly one new page resident (D-011).

### L3 — Heap (`src/heap.{h,cpp}`)
**Phase 1 (built):** one process-wide heap `g_heap`, statically initialised with every `current[c]` pointing at the sentinel `g_empty_span` (whose `free` list is always null). Every call takes one global spin lock in `alloc.cpp` (D-021). The span queue per class is a doubly linked list of spans that are not full:
- a full span leaves the queue (detected in the slow path) and rejoins at the **back** when one of its blocks is freed, so the current span keeps serving;
- a span whose last block is freed is **retired** (pages back to the segment) unless it is the class's current span, which is kept to avoid churn in malloc/free loops (D-024).

**Phase 2 (design):**
```
struct Heap {
  Span*    current[NUM_CLASSES];  // the span malloc tries first, per size class
  SpanQueue queue[NUM_CLASSES];   // other spans of this class with free space
  Segment* segments;              // segments this heap owns
  Stats    stats;                 // counters (allocations, frees, remote frees ...)
};
static thread_local Heap* tl_heap [[gnu::tls_model("initial-exec")]] = &empty_heap;
```

- **One heap per thread**, created on that thread's first `malloc`. Its memory comes from Arena's own bootstrap segment, never from `malloc` (D-012).
- `tl_heap` starts out pointing to a static **sentinel** `empty_heap` whose spans are always "full". The fast path therefore never needs an `if (heap == nullptr)` check; the first allocation simply falls to the slow path, which creates the real heap (D-013).
- The `initial-exec` TLS model makes `tl_heap` a single register-relative load. The default model can call `__tls_get_addr`, which may call `malloc` and recurse forever (D-014).
- **Thread exit:** a `pthread_key_create` destructor runs. Spans that still hold live blocks are marked *abandoned* and put on a global abandoned list, the only lock in Arena, and only on the slow path. Another thread can later adopt them. Empty spans and segments are released (D-015).

### L4 — Core API (`include/arena/arena.h`)
Plain C functions with an `arena_` prefix: `arena_malloc`, `arena_free`, `arena_calloc`, `arena_realloc`, `arena_memalign`, `arena_usable_size`, `arena_version` (`arena_stats_print` comes in Phase 3). They and the shim both call hidden `arena::*_impl` functions (`src/api.h`), so the shim's calls are direct, not through the PLT (D-026).

- `realloc` stays in place when the new size fits and wastes at most half the block (blocks ≤ 128 B always stay); `realloc(p, 0)` frees and returns NULL, like glibc (D-025).
- `calloc` checks `count * size` for overflow and zeroes outside the lock.
- `memalign`: see §4a.

These are what the tests, TSan stress tests and fuzzers call. TSan and ASan *themselves* replace `malloc`, so they cannot be combined with `LD_PRELOAD`. Testing through a prefixed API avoids that conflict (D-003).

### L5 — Shim (`src/shim.cpp`)
Exports the real glibc names and forwards each to the core: `malloc`, `free`, `calloc`, `realloc`, `reallocarray`, `posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `pvalloc`, `malloc_usable_size`.

C++ `operator new` and `delete` in libstdc++ call `malloc` and `free`, so they work without extra code. Sized `delete` is a later optimisation.

Only the shared library `libarena.so` contains the shim. It is linked with `-nostdlib++ -Wl,-z,defs`, so any dependency on libstdc++ (which allocates) is a link error rather than a runtime recursion (D-026). The aligned family was built in Phase 1, not Phase 3 as first planned, because D-002 requires replacing the whole family before any real program runs (D-028).

---

## 4. Size classes (D-007, D-008)

All blocks are multiples of 16 bytes, so every pointer is 16-byte aligned, which satisfies `alignof(max_align_t)` on x86-64 and AArch64.

| Range | Step | Classes |
|---|---|---|
| 16 – 128 B | 16 B | 8 |
| 128 B – 32 KiB | 4 classes per power of two (e.g. 160, 192, 224, 256) | 32 |
| **Total small classes** | | **40** |
| 32 KiB – 1 MiB | *large*: a span of whole 64 KiB pages | — |
| > 1 MiB | *huge*: a dedicated segment from `mmap` | — |

- **Worst-case internal waste is under 25%.** Rounding 129 B up to 160 B wastes 31 B, which is about 19%.
- **Size → class lookup** (`src/size_class.h`, D-019): sizes up to 128 B use `(size - 1) >> 4`. Larger sizes use the position of the highest set bit of `size - 1` (`clz`) plus the 2 bits below it, which pick the quarter of that power-of-two range. A few instructions, no loop, no table.
- Spans for classes up to 8 KiB are one 64 KiB page. Larger small classes use more pages so that each span holds at least 8 blocks.

## 4a. Aligned allocation (D-022)
- **alignment ≤ 16:** plain `malloc`.
- **16 < alignment ≤ 64 KiB:** round the size up to a multiple of the alignment. Spans start 64 KiB-aligned and block *i* is at `start + i × block_size`, so a block is aligned whenever its class size is a multiple of the alignment. If the class isn't, use the next power of two: every power of two from 16 B to 32 KiB is a class. Large and huge blocks start 64 KiB-aligned anyway. **Result: small pointers are always block starts**, so `free` never has to round a pointer down.
- **64 KiB < alignment ≤ 2 MiB:** over-allocate a large/huge block and return an aligned pointer inside it. `free` and `usable_size` find it through the page table (`head` is set for every page).
- **> 2 MiB:** returns NULL (`ENOMEM`/`EINVAL`). The returned pointer must stay inside the first 4 MiB of its segment for `segment_of()` to work.

---

## 5. The life of a pointer

> Phase 1: every call below runs inside the global lock (D-021), and step 2 of the slow path (`thread_free`) and the remote-free case don't exist yet.

### `malloc(40)` — fast path (expected about 95%+ of calls)
```
cls  = size_to_class(40)          // class 48 B
span = tl_heap->current[cls]
blk  = span->free                 // non-null on the fast path
span->free = blk->next
span->used++
return blk
```
No lock, no atomic, no system call.

### `malloc` — slow path (`span->free` is empty)
0. If `current` is the sentinel (no spans for this class yet), make a new span and put it at the front of the queue.
1. Move `local_free` into `free`. This is a pointer swap.
2. Collect `thread_free`: `exchange(nullptr)`, then splice the whole list into `free`.
3. Still empty? Extend the free list from the span's untouched memory.
4. Still empty? Move to the next span in `queue[cls]` that has space. Full spans leave the queue.
5. No span left? Take free pages from a segment this heap owns, or adopt an abandoned span, or get a new segment from the OS (L0).

### `free(p)` — same thread
```
seg  = p & ~SEGMENT_MASK
span = seg->pages[(p - seg) >> 16].span_head
if (span->owner == tl_heap) { push p on span->local_free; span->used--; }
```

### `free(p)` — different thread (a remote free)
```
else  CAS-push p onto span->thread_free     // lock-free, multiple producers
```
The owner reclaims these blocks in slow-path step 2. When a span's `used` count reaches 0, its pages go back to the segment and may later be decommitted (`os_decommit`).

---

## 6. Hazards specific to writing a `malloc` (read before coding)

These are the classic ways a `malloc` replacement breaks. Each one is a likely interview question.

| Hazard | What goes wrong | Guard |
|---|---|---|
| **Recursion** | Arena calls something that calls `malloc` (e.g. `printf`, `dlsym`, `std::vector`, `std::string`, `pthread_setspecific` on a high key) and recurses into itself forever. In Phase 1 this would *deadlock* on the global lock rather than recurse | No libc that allocates inside Arena. Write output with `write(2)` and Arena's own number formatter. No STL containers (D-004). `libarena.so` is linked `-nostdlib++ -z defs`, so a libstdc++ dependency fails the build (D-026) |
| **Compiler rewriting** | The compiler sees `malloc` followed by `memset(0)` and turns it into a call to `calloc`. Inside `calloc`, that means infinite recursion | Build with `-fno-builtin` (D-004) |
| **TLS initialisation** | Dynamic TLS may call `malloc` | `initial-exec` TLS model (D-014) |
| **Allocation before `main`** | `ld.so` and libc call `malloc` before any constructor has run | Lazy initialisation inside the first `malloc`; no reliance on static constructors (D-013) |
| **`fork()` while a thread holds the abandoned-list lock** | The child process deadlocks | `pthread_atfork` handlers (Phase 3) |
| **Foreign pointers** | `free()` receives a pointer Arena didn't allocate (e.g. memory from before `LD_PRELOAD` took effect) | A segment registry check in debug mode. In release, this is documented as undefined |
| **`realloc`, `calloc` edge cases** | `calloc(n, m)` overflows; `realloc(p, 0)`; `realloc(NULL, n)` | Explicit tests for each POSIX rule (Phase 3) |

---

## 7. Debug mode (`-DARENA_DEBUG=ON`)
- Freed blocks are filled with `0xDF`, so use-after-free reads show up as garbage you can spot.
- Free-list `next` pointers are XOR-encoded with a random key, so a corrupted list is detected rather than followed.
- Double-free check: is the block already on a free list of its span?
- Huge allocations get a `PROT_NONE` **guard page** after them, so overflows crash right away.
- Every check costs time. None of them are compiled into release builds.

---

## 8. Observability
- Per-heap counters: allocations, frees, local vs remote frees, slow-path hits, segments mapped and decommitted.
- `ARENA_STATS=1` prints a summary at exit, using `write(2)` only.
- These counters are what the Phase 4 write-up uses to explain *why* Arena wins or loses on each workload.

---

## 9. Repository layout
```
arena/
├── CLAUDE.md                 instructions for Claude (kept up to date)
├── LOG.md                    chronological work log
├── CMakeLists.txt
├── docker/Dockerfile         Linux dev environment (Ubuntu 24.04)
├── include/arena/arena.h     core API
├── src/  config.h os.{h,cpp} size_class.h span.h segment.{h,cpp} heap.{h,cpp}
│         api.h alloc.cpp shim.cpp check_internal.h          (stats.cpp: Phase 3)
├── tests/ check.h  unit/  fuzz/fuzz_shadow.cpp               (stress/: Phase 2)
├── bench/ latency/ scaling/                                  (Phase 4)
├── scripts/ dev.sh check.sh preload_smoke.sh mutation_check.sh
│            (real_programs.sh: Phase 3, mimalloc_bench.sh: Phase 4)
└── docs/  ARCHITECTURE.md ROADMAP.md NOTEBOOK.md  results/
```
Build outputs: `libarena.so`, which contains the shim and is used with `LD_PRELOAD`, and `libarena_core.a`, which has the core API only and is used by the tests.
