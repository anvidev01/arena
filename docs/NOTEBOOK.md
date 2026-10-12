# Arena — Engineering Notebook

This is the study companion for Arena. It records **every design decision** (why it was made, what else was considered, what it costs) and **every bug** (what happened, how it was found, the root cause, the fix with code, and the lesson).

Read an entry, then open the code it points to. You should be able to explain both out loud.

- Decisions are numbered **D-001, D-002, …** and never renumbered. If a decision is reversed, mark it *Superseded by D-0xx* and add the new entry; do not delete the old one. How a design changed is a good interview story.
- Bugs are numbered **B-001, B-002, …**
- The chronological "what happened when" goes in [../LOG.md](../LOG.md). This file is for the *why*.

---

## Index

| ID | Title | Status |
|---|---|---|
| D-001 | Linux only; develop in a Docker container on the Mac | Accepted |
| D-002 | Replace `malloc` with `LD_PRELOAD` symbol interposition | Accepted |
| D-003 | Two build outputs: prefixed core API + exporting shim | Accepted |
| D-004 | C++20 subset: no exceptions, no RTTI, no STL containers, `-fno-builtin` | Accepted |
| D-005 | 4 MiB segments aligned to 4 MiB | Accepted (revisit in Phase 4) |
| D-006 | Get alignment by over-mapping and trimming | Accepted |
| D-007 | Minimum block 16 B; all blocks 16-byte aligned | Accepted |
| D-008 | 40 small size classes, 4 per power of two | Accepted (revisit in Phase 4) |
| D-009 | Three free lists per span (free list sharding) | Accepted |
| D-010 | Remote frees: CAS push, whole-list `exchange` to take | Accepted |
| D-011 | Lazy span initialisation | Accepted |
| D-012 | Thread heaps allocated from Arena's own memory | Accepted |
| D-013 | `empty_heap` sentinel and lazy initialisation | Accepted |
| D-014 | `initial-exec` TLS model for the heap pointer | Accepted |
| D-015 | Abandon and adopt spans on thread exit | Accepted |
| D-016 | Tiny in-house test runner instead of GoogleTest | Accepted |
| D-017 | Correctness in Docker; benchmark numbers on dedicated x86-64 Linux | Accepted |
| D-018 | Latency measured over batches with `clock_gettime` / `rdtsc` | Proposed |
| D-019 | Size → class lookup by arithmetic only (no table) | Accepted (revisit in Phase 4) |
| D-020 | Dev container via `scripts/dev.sh`; one script runs all four build configs | Accepted |
| D-021 | Phase 1: one global heap behind one spin lock | Accepted, temporary (removed in Phase 2) |
| D-022 | Aligned allocation without interior pointers for small blocks | Accepted |
| D-023 | Page 0 holds only the segment header; first-fit page bitmap | Accepted (revisit in Phase 4) |
| D-024 | Span retirement and segment release policy | Accepted (revisit in Phase 4) |
| D-025 | `realloc` in-place rule; glibc semantics for `malloc(0)` / `realloc(p, 0)` | Accepted |
| D-026 | Shim calls hidden `*_impl` functions; `libarena.so` linked `-nostdlib++ -z defs` | Accepted |
| D-027 | Invalid-free check in every build; invariant asserts in Debug | Accepted |
| D-028 | Build the whole aligned family in Phase 1 (moved from task 3.1) | Accepted |
| D-029 | Mutation testing: prove the tests can fail | Accepted |

---

## Part 1 — Decisions

### D-001 — Linux only; develop in a Docker container on the Mac
- **Date:** 2026-10-06
- **Context:** the development machine is an Apple M4 Mac (macOS 15.7). The project's purpose is the Linux `malloc` interface, and mimalloc-bench, `perf` and `LD_PRELOAD` are all Linux tools.
- **Decision:** target Linux only (x86-64 and AArch64). Develop inside an Ubuntu 24.04 Docker container on the Mac, which runs as native AArch64 Linux.
- **Alternatives:** (a) support macOS too. Rejected: macOS replaces allocators through *malloc zones* and `DYLD_INSERT_LIBRARIES`, which is a different mechanism and would double the work. (b) A full VM (UTM). Rejected: heavier, and Docker is already installed.
- **Cost:** Docker Desktop runs a Linux VM, so timing measurements are noisy, and hardware performance counters may not be available. See D-017.
- **Interview line:** "I targeted the Linux allocator ABI because that's where `LD_PRELOAD` interposition and the standard benchmark suite live."

### D-002 — Replace `malloc` with `LD_PRELOAD` symbol interposition
- **Date:** 2026-10-06
- **How it works:** the dynamic linker (`ld.so`) resolves each symbol to the *first* library that defines it. `LD_PRELOAD=./libarena.so` puts Arena ahead of glibc, so every `malloc` call in the program, *including calls made inside glibc*, goes to Arena.
- **Alternatives:** (a) `__malloc_hook`. Rejected: removed in glibc 2.34. (b) Linking statically into each program. Rejected: we want to run unmodified binaries.
- **Consequence:** Arena must provide the *whole* family (`malloc`, `free`, `calloc`, `realloc`, the aligned variants, `malloc_usable_size`). If one is missing, glibc's version gets a pointer it never allocated and crashes.
- **Interview line:** symbol interposition and lookup order.

### D-003 — Two build outputs: prefixed core API + exporting shim
- **Date:** 2026-10-06
- **Decision:** `libarena_core.a` exposes `arena_malloc` and related functions. `libarena.so` adds `shim.cpp`, which exports the real names `malloc` and so on.
- **Why:**
  1. **Sanitizers.** TSan and ASan install their *own* `malloc` interceptors, so they cannot run with Arena preloaded. With prefixed names, a test program uses the system `malloc` (instrumented by the sanitizer) and calls Arena explicitly. TSan then checks *Arena's* code for data races.
  2. **The fuzzer needs a second allocator.** Its shadow map (`std::map`) must allocate from a different allocator than the one under test, or it would be checking itself with itself.
- **Cost:** one extra forwarding layer. It is inlined in release builds, so it costs nothing at runtime.

### D-004 — C++20 subset: no exceptions, no RTTI, no STL containers, `-fno-builtin`
- **Date:** 2026-10-06
- **Decision:** write Arena in C++20, using `std::atomic`, `constexpr`, templates and `[[likely]]`. Do **not** use exceptions, RTTI, `std::vector`, `std::string`, `std::map`, `iostream` or `printf` inside the allocator. Compile with `-fno-builtin`.
- **Why:**
  - Anything that allocates calls `malloc`, which is Arena itself: infinite recursion. STL containers and `printf` can allocate.
  - Exceptions need allocated exception objects and unwinding tables. A `malloc` must never throw.
  - `-fno-builtin` stops the compiler from "optimising" a `malloc` followed by a `memset` to zero into a call to `calloc`. Inside our own `calloc`, that is infinite recursion. This is a known real-world bug in custom allocators.
- **Alternative:** plain C. Rejected: `std::atomic` with explicit memory orders and `constexpr` size-class tables are clearer and safer in C++.

### D-005 — 4 MiB segments aligned to 4 MiB
- **Date:** 2026-10-06
- **Decision:** get memory from the OS in 4 MiB segments whose start address is a multiple of 4 MiB. Each segment is split into 64 pages of 64 KiB.
- **Why:** `free(p)` has to find `p`'s metadata quickly. With aligned segments, `p & ~(4MiB-1)` gives the segment header directly, with no search and no global table. A shift then gives the page index.
- **Alternatives:** (a) a header before every block, as in glibc. Rejected: wastes 8–16 bytes per allocation and mixes metadata with user data, so a buffer overflow corrupts the allocator. (b) A global radix tree from page to span, as in tcmalloc's pagemap. Rejected: more memory and more lookups. Worth knowing for the interview.
- **Cost:** a segment holds up to 4 MiB of address space even when lightly used. Most of it is never touched, so it is *virtual* rather than physical memory, and RSS stays low (D-011). Revisit the 4 MiB / 64 KiB values in Phase 4.

### D-006 — Get alignment by over-mapping and trimming
- **Date:** 2026-10-06
- **Decision:** to get a 4 MiB-aligned 4 MiB region, `mmap` 8 MiB, find the aligned address inside it, and `munmap` the unused head and tail.
- **Why:** `mmap` only guarantees page alignment. This trick wastes no memory once trimmed.
- **Optimisation for later:** first try `mmap` with an address hint just past the previous segment; it is often already aligned.

### D-007 — Minimum block 16 B; all blocks 16-byte aligned
- **Date:** 2026-10-06
- **Why:** `malloc` must return memory aligned for any standard type: `alignof(max_align_t)` = 16 on x86-64 and AArch64 Linux. Every class being a multiple of 16 guarantees this for free. 16 B also leaves room for the intrusive `next` pointer (8 B), plus 8 B for an encoded check value in debug mode.
- **Cost:** `malloc(1)` through `malloc(8)` use 16 bytes. glibc's minimum chunk is 32 B, so this is still smaller.

### D-008 — 40 small size classes, 4 per power of two
- **Date:** 2026-10-06
- **Decision:** 16-byte steps up to 128 B, then 4 classes per doubling up to 32 KiB (40 classes in all). Larger sizes are *large* (a whole-page span) or *huge* (a dedicated segment) above 1 MiB.
- **Why:** this caps internal waste below 25%. Fewer classes would waste more memory. More classes would spread blocks across more spans, raising RSS and lowering cache reuse. jemalloc and mimalloc use similar spacing.
- **Lookup:** ~~a byte table for sizes up to 1 KiB; `clz` arithmetic above that.~~ Superseded by D-019 (arithmetic only).

### D-009 — Three free lists per span (free list sharding)
- **Date:** 2026-10-06
- **Decision:** each span keeps `free` (allocation pops), `local_free` (owner frees) and `thread_free` (atomic, remote frees).
- **Why:**
  - Keeping frees off the list `malloc` pops from makes the fast path a single pop until `free` empties. That moment is a natural, bounded point for housekeeping (collecting remote frees, retiring full spans), instead of checking on every call.
  - Remote frees go on their own atomic list, so the owner's lists need no atomics at all.
  - Many small per-span lists keep related blocks close together in memory, which improves locality compared with one big list per thread.
- **Source:** Leijen, Zorn, de Moura, *"Mimalloc: Free List Sharding in Action"* (2019). Read sections 2–3.

### D-010 — Remote frees: CAS push, whole-list `exchange` to take
- **Date:** 2026-10-06
- **Decision:** other threads push freed blocks with a CAS loop (`release` ordering). The owner takes the whole list with `thread_free.exchange(nullptr, acquire)`.
- **Why no ABA problem:** ABA happens when a consumer reads `head = A` and `A->next = B`, gets delayed while A is popped, freed and pushed back, then succeeds a CAS from A to B even though B is stale. Here the consumer *never reads `next` before an atomic operation*: it swaps the whole list out in one step. Producers only push, and a push CAS stays correct even if the head changed and changed back.
- **Memory ordering:** the producer's writes to the block (its `next` field) must be visible to the owner before the owner walks the list. `release` on the push and `acquire` on the exchange provide exactly that.
- **Alternative:** a lock per span. Rejected: a lock on every remote free hurts producer–consumer workloads.

### D-011 — Lazy span initialisation
- **Date:** 2026-10-06
- **Decision:** a new span does not link all its blocks into a free list. It keeps a bump pointer into untouched memory and extends the free list a little at a time.
- **Why:** writing a `next` pointer into every block *touches* every page. The kernel then backs each one with physical memory (first-touch page faults), even if the program only needed one block. Lazy filling keeps RSS proportional to real use.

### D-012 — Thread heaps allocated from Arena's own memory
- **Date:** 2026-10-06
- **Why:** if `Heap` objects came from `malloc`, creating a thread's heap would call `malloc`, which needs the thread's heap: recursion. Heaps are fixed-size, so they come from a small dedicated region (a free list of `Heap` slots in a bootstrap segment).

### D-013 — `empty_heap` sentinel and lazy initialisation
- **Date:** 2026-10-06
- **Decision:** `tl_heap` initially points to a static, read-only `empty_heap` whose `current[cls]` spans are permanently "full". The very first `malloc` on any thread fails the fast path, enters the slow path, and that is where the global state and the thread's real heap are initialised.
- **Why:**
  - Removes an `if (heap == nullptr)` branch from every `malloc`.
  - Works before `main()` and before any static constructor. `ld.so` and libc call `malloc` very early, so Arena cannot depend on a constructor having run.

### D-014 — `initial-exec` TLS model for the heap pointer
- **Date:** 2026-10-06
- **Why:** in a shared library, the default (`global-dynamic`) TLS model reads a thread-local through `__tls_get_addr`, which may allocate memory on first access: recursion. `initial-exec` makes the access a fixed offset from the thread pointer register (`fs` on x86-64, `tpidr_el0` on AArch64). This works for `LD_PRELOAD` libraries because they are loaded at program start, not through `dlopen`.

### D-015 — Abandon and adopt spans on thread exit
- **Date:** 2026-10-06
- **Decision:** when a thread exits, a `pthread_key` destructor releases empty spans and segments. Spans that still hold live blocks (freed later by other threads) go onto a global *abandoned* list with no owner. A thread whose allocation slow path finds no space checks that list and adopts spans.
- **Why:** blocks can outlive the thread that allocated them, especially in producer–consumer patterns. Releasing such spans would break those blocks, and keeping them forever would leak memory.
- **Cost:** the abandoned list is Arena's only lock. It is touched only on thread exit and on the slow path.

### D-016 — Tiny in-house test runner instead of GoogleTest
- **Date:** 2026-10-06
- **Why:** each test is a small program with `CHECK(cond)` macros, run by `ctest`. There are no external dependencies, and builds are quick in the container. GoogleTest would work, since D-003 means tests use the system `malloc`, but it adds a dependency without real benefit at this size. Revisit if the tests grow large.

### D-017 — Correctness in Docker; benchmark numbers on dedicated x86-64 Linux
- **Date:** 2026-10-06
- **Decision:** develop and test correctness in Docker on the M4. The *published* numbers come from a dedicated x86-64 Linux machine (e.g. a bare-metal or dedicated cloud instance), with CPU frequency fixed and threads pinned.
- **Why:** Docker Desktop on macOS runs a VM with noisy timing and limited access to performance counters. Most HFT firms run x86-64, and jemalloc and mimalloc are most tuned there, so x86-64 numbers compare fairly.
- **Open:** pick the machine in Phase 4 and record its exact model in `docs/results/`.

### D-018 — Latency measured over batches with `clock_gettime` / `rdtsc` *(Proposed)*
- **Date:** 2026-10-06
- **Problem:** a single `malloc` takes about 10–20 ns. On AArch64 the generic timer (`cntvct_el0`) often ticks at only about 24 MHz, roughly 41 ns per tick, which is coarser than the thing being measured.
- **Proposal:** on x86-64, use `rdtscp` with serialising fences for each call. On AArch64, time batches of calls and report per-call averages, labelled clearly as batch averages. Finalise in Phase 4.

### D-019 — Size → class lookup by arithmetic only (no table)
- **Date:** 2026-10-06 · **Supersedes:** the lookup line of D-008
- **Decision:** `size_to_class` is pure arithmetic. For sizes up to 128 B, `(size - 1) >> 4`. Above that, let `s = size - 1` and `top` = index of its highest set bit (`63 - clz(s)`). The two bits just below `top` say which quarter of the range `(2^top, 2^(top+1)]` the size is in. So `class = 8 + (top - 7) * 4 + quarter`.
- **Why:** it is equally constant-time, and a table would need generating, plus a cache line on the hot path. The arithmetic can be proven correct by testing all 32,768 sizes, which `test_size_class` does.
- **Worked example:** `size = 200` gives `s = 199 = 0b1100_0111`, so `top = 7`. The two bits under it are `10` = 2, so the class is 8 + 0 + 2 = 10, which is 224 B. Check: 192 < 200 ≤ 224. ✓
- **Revisit:** Phase 4 can compare it with a table version in a microbenchmark.

### D-020 — Dev container via `scripts/dev.sh`; one script runs all four build configs
- **Date:** 2026-10-06
- **Decision:** `scripts/dev.sh <cmd>` runs any command in the `arena-dev` image (building it on first use), with the repo mounted at `/arena`. `scripts/check.sh [release|debug|asan|tsan|all]` configures, builds and runs `ctest` for each configuration in `build/<config>`.
- **Sanitizers are a CMake option** (`ARENA_SANITIZE=address|thread`), not separate build types, so a sanitizer build can also be optimised (TSan is far faster at `-O2`).
- **Container flags:** `--cap-add=SYS_PTRACE` for gdb; `--security-opt seccomp=unconfined` because TSan re-executes itself with ASLR disabled through `personality()`, which Docker's default seccomp profile blocks.

### D-021 — Phase 1: one global heap behind one spin lock
- **Date:** 2026-10-09 · **Temporary:** replaced by per-thread heaps in Phase 2
- **Decision:** Phase 1 has a single heap (`g_heap` in `src/heap.cpp`). Every `arena_*` call takes one test-and-test-and-set spin lock (`SpinLock` in `src/alloc.cpp`) that yields the CPU while it waits.
- **Why:** under `LD_PRELOAD`, Arena runs inside programs that *do* create threads (Python, gcc's driver, Redis). Without a lock, a single-threaded design would corrupt memory the moment two threads called `malloc`. The lock makes Phase 1 correct everywhere, so real-program testing can start now.
- **Why a spin lock and not `pthread_mutex`:** the critical sections are short (tens of nanoseconds), and a spin lock is one `std::atomic<bool>` that needs no initialisation, which matters because `malloc` runs before `main()`.
- **Cost:** every call does an atomic exchange and an atomic store (roughly 10–20 ns uncontended), and threads serialise. This is exactly what Phase 2 removes, and the Phase 4 numbers must come from the Phase 2 design, never this one.
- **Danger to know:** if anything inside Arena called `malloc` while holding the lock, the program would **deadlock** (the lock isn't recursive), not recurse. That is one more reason for D-004 and D-026.

### D-022 — Aligned allocation without interior pointers for small blocks
- **Date:** 2026-10-09
- **Problem:** the usual way to implement `memalign(A, n)` is to allocate `n + A − 1` bytes and return an aligned pointer *inside* the block. Then `free(p)` receives a pointer that is not a block start and must round it down, which costs a division in `free`, or a per-span flag checked on every free.
- **Decision** (`heap_memalign` in `src/heap.cpp`):
  - `A ≤ 16`: plain `malloc`.
  - `16 < A ≤ 64 KiB`: request `n` rounded up to a multiple of `A`. Small spans start 64 KiB-aligned and block *i* starts at `start + i × block_size`, so the block is A-aligned whenever `block_size % A == 0`. If the chosen class doesn't satisfy that, round `n` up to the next power of two. Every power of two from 16 B to 32 KiB is a class, and a power of two ≥ A is a multiple of A. Large/huge blocks start 64 KiB-aligned anyway. **So small pointers are always block starts, and the `free` fast path is unchanged.**
  - `64 KiB < A ≤ 2 MiB`: over-allocate a large/huge block and return an interior pointer. That is fine here, because `free` of a large/huge block uses `span->start`, and every page descriptor's `head` finds the span.
  - `A > 2 MiB`: unsupported, returns NULL. A huge block starts 64 KiB into its segment, so the aligned pointer could land beyond the first 4 MiB, where `segment_of()` would find the wrong "segment". glibc supports this; real programs essentially never ask for it.
- **Cost:** up to 2× rounding waste for odd sizes with large alignments, e.g. `memalign(4096, 4097)` uses an 8 KiB block.
- **Worked example:** `memalign(32, 40)` → n = 64 → class 64, and 64 % 32 = 0, so the block is aligned. `memalign(32, 70)` → n = 96 → class 96, and 96 % 32 = 0 ✓. `memalign(64, 70)` → n = 128 → class 128 ✓.

### D-023 — Page 0 holds only the segment header; first-fit page bitmap
- **Date:** 2026-10-09
- **Decision:** the `Segment` header (links, size, a 64-bit free-page bitmap and 64 `Span` descriptors, about 6 KiB) gets all of page 0; pages 1–63 are usable. Page runs are found first-fit by sliding an n-bit mask across the bitmap.
- **Why:** simple pointer math and no special case for "page 0 is shorter". The unused ~58 KiB of page 0 are never touched, so they cost *virtual* address space, not RAM. mimalloc uses the rest of page 0; that is a possible Phase 4 refinement.
- **Why first-fit with a bitmap:** at most 63 positions to check, slow path only. Best-fit would fragment less but isn't worth the complexity before measurements show a need.

### D-024 — Span retirement and segment release policy
- **Date:** 2026-10-09
- **Decision:**
  1. When a small span's last block is freed and it is **not** the class's `current` span, it is retired at once: its pages go back to the segment.
  2. If it **is** `current`, it is kept, so `for (...) free(malloc(n));` reuses one span instead of creating and releasing one every iteration.
  3. A segment is unmapped as soon as it is empty, unless it is the heap's only segment.
  4. Freed pages are not decommitted (`madvise`) yet.
- **Why:** returning memory promptly keeps RSS honest, and rules 2 and 3 stop the two obvious thrash patterns.
- **Found by mutation testing (D-029):** removing rule 2 does *not* break correctness. The fuzzer passed, because `retire_span` also removes the span from the queue and resets `current`. So rule 2 is a performance rule, now guarded by `test_alloc: malloc_free_ping_pong_does_not_churn_spans`.
- **Revisit in Phase 4:** decommit delay, keeping more than one empty segment, and what mimalloc-bench's fragmentation workloads show.

### D-025 — `realloc` in-place rule; glibc semantics for `malloc(0)` / `realloc(p, 0)`
- **Date:** 2026-10-09
- **Decision:**
  - `malloc(0)` returns a unique 16-byte block (glibc does the same; some programs depend on non-NULL).
  - `realloc(p, 0)` frees `p` and returns NULL, matching glibc. The C standard leaves this implementation-defined, so matching the system allocator avoids surprising programs.
  - `realloc` stays in place if the new size fits and is at least half the usable size; blocks ≤ 128 B always stay. Otherwise it allocates, copies `min(old usable, new)` bytes and frees the old block. On failure the old block is untouched, as the standard requires.
- **Why "at least half":** shrinking a 1 MiB block to 10 bytes in place would pin 1 MiB for a 10-byte object.

### D-026 — Shim calls hidden `*_impl` functions; `libarena.so` linked `-nostdlib++ -z defs`
- **Date:** 2026-10-09
- **Decision:** `malloc` (shim) and `arena_malloc` (core API) both call `arena::malloc_impl`, which has hidden visibility. `libarena.so` is linked with `-nostdlib++` and, in non-sanitizer builds, `-Wl,-z,defs`.
- **Why hidden `*_impl`:** if the shim called the exported `arena_malloc`, the call would go through the PLT, and another library could even interpose it. Hidden functions are plain direct calls.
- **Why `-nostdlib++`:** libstdc++ allocates (e.g. for exceptions and iostreams). If any Arena code started depending on it, the link would now **fail**, instead of the program deadlocking at runtime (D-021). `-z defs` turns any unresolved symbol into a link error. Verified: `ldd libarena.so` lists only libc, libm and libgcc_s, and `nm -D` shows exactly the 18 intended exports.

### D-027 — Invalid-free check in every build; invariant asserts in Debug
- **Date:** 2026-10-09
- **Decision:**
  - `heap_free` aborts with "invalid pointer or double free" when the pointer's page has no span (`head == nullptr`). It costs one well-predicted branch, so it stays in release. It catches double frees of large/huge blocks and frees of pointers into released pages. (It cannot catch a double free of a *small* block; that needs the Phase 3 debug mode.)
  - `ARENA_ASSERT` (`src/check_internal.h`) checks invariants in Debug and `ARENA_DEBUG` builds and compiles to nothing in release. Current checks: `used > 0` before a small free, and in the slow path "`current` is the head of its queue".
- **Why the second assert exists:** mutation M5 (D-029) made `current` dangle, and the release build simply **hung** in an endless slow-path loop. With the assert, a Debug build stops at once with `arena: malloc: current span is not the head of its queue`. An invariant check turns a hang into a message that names the broken rule.

### D-028 — Build the whole aligned family in Phase 1 (moved from task 3.1)
- **Date:** 2026-10-09
- **Decision:** `posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `pvalloc` and `reallocarray` were written in Phase 1, not Phase 3.
- **Why:** D-002 requires replacing the *whole* family. If `posix_memalign` were missing, glibc would allocate the block from *its* heap, and our `free` would then be handed a pointer it never allocated. Real programs call these (the `gcc` test in `preload_smoke.sh` uses `posix_memalign`), so task 1.8 needed them.
- **What stays in Phase 3 (3.1–3.2):** systematic POSIX edge-case tests (`errno` values, `EINVAL` cases).

### D-029 — Mutation testing: prove the tests can fail
- **Date:** 2026-10-09
- **Decision:** `scripts/mutation_check.sh` plants known bugs one at a time in a throwaway copy of the tree and requires the tests to catch each one. Run it after changing the allocator or the tests.
- **Why:** a test suite that passes proves nothing until you've seen it fail on a real bug. This also found two real gaps (D-024 rule 2 was unguarded; M5 hung instead of failing) and one fragile test (B-004).
- **Result, 2026-10-09: 6 / 6 caught.**

  | # | Planted bug | Caught by | First report |
  |---|---|---|---|
  | M1 | bump pointer one block short: a block is handed out twice | fuzzer | `op 24: overlaps the next live block` |
  | M2 | `realloc` copies one byte too few | fuzzer | `op 35: contents changed` |
  | M3 | `calloc` doesn't zero | fuzzer | `op 3: calloc memory not zero` |
  | M4 | empty `current` span released (performance rule) | `test_alloc` | `spans_created - before == 0` failed |
  | M5 | `queue_remove` leaves `current` dangling | Debug assert | `arena: malloc: current span is not the head of its queue` |
  | M6 | `size_to_class` picks the wrong quarter | `static_assert` | compile error |

---

## Part 2 — Bug Journal

Every bug, however small, gets an entry using the template at the end of this file. Each bug also gets a **regression test** that fails before the fix and passes after it.

### B-001 — ASan build fails to link: sanitizer runtime missing from the container
- **Date found / fixed:** 2026-10-06 / 2026-10-06
- **Phase / component:** Phase 0 / `docker/Dockerfile`
- **Symptom:** release and debug builds passed. The ASan build failed while linking `libarena.so`:
  ```
  /usr/bin/ld: cannot find /usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.asan_static-aarch64.a: No such file or directory
  clang++: error: linker command failed with exit code 1
  ```
- **How to reproduce:** build the image from the first Dockerfile, then `scripts/dev.sh scripts/check.sh asan`.
- **How we found the cause:**
  1. First guess: Ubuntu's aarch64 LLVM package ships ASan without the `asan_static` piece, a platform packaging bug. Checking that meant listing the directory.
  2. `ls /usr/lib/llvm-18/lib/clang/18/lib/linux/` failed: **the whole directory did not exist**, so this wasn't one missing file. *No* sanitizer runtime was installed, which also meant TSan would have failed.
  3. The runtimes live in the `libclang-rt-18-dev` package, and it was not installed. In Debian/Ubuntu, `clang-18` only *Recommends* `libclang-rt-18-dev`, and the Dockerfile uses `--no-install-recommends` to keep the image small.
- **Root cause:** `--no-install-recommends` removed a package we needed, because we relied on clang to pull it in rather than listing it ourselves.
- **Fix:** list the package explicitly in the Dockerfile.
  ```dockerfile
  # before
  RUN apt-get update && apt-get install -y --no-install-recommends \
          build-essential clang lld cmake ninja-build \
  # after
  # libclang-rt-18-dev holds the ASan/TSan runtimes. It is only a *Recommends* of clang,
  # so --no-install-recommends silently drops it unless listed explicitly (B-001).
  RUN apt-get update && apt-get install -y --no-install-recommends \
          build-essential clang lld cmake ninja-build \
          libclang-rt-18-dev \
  ```
- **Regression test:** `scripts/check.sh all` builds and runs the `asan` and `tsan` configurations; a missing runtime fails the link immediately. (It must be run after any Dockerfile change.)
- **Lesson / interview angle:** list every dependency you actually use; don't rely on a package manager's optional "recommends". Also: **check the obvious assumption ("is it installed at all?") before blaming the platform.** The first guess here was wrong and would have sent us looking for a workaround to a bug that didn't exist.
- **Commit:** (first commit, pending)

### B-002 — Test read a span descriptor after `segment_free_pages` wiped it
- **Date found / fixed:** 2026-10-09 / 2026-10-09 (found in self-review, before the first run)
- **Phase / component:** Phase 1 / `tests/unit/test_segment.cpp`
- **Symptom:** none observed; it would have been a segfault. The test did:
  ```cpp
  segment_free_pages(seg, three);
  CHECK(span_of(three->start) == nullptr);
  ```
- **Root cause:** `three` is not the memory; it points at the span's **descriptor** in the segment header. `segment_free_pages` resets that descriptor to `Span{}`, so `three->start` became `nullptr`. Then `span_of(nullptr)` computes `segment_of(0) == 0` and reads address 0.
- **Fix:**
  ```cpp
  // before
  segment_free_pages(seg, three);
  CHECK(span_of(three->start) == nullptr);
  // after
  std::uint8_t* three_start = three->start;  // the descriptor is wiped by free_pages
  segment_free_pages(seg, three);
  CHECK(span_of(three_start) == nullptr);
  ```
- **Regression test:** the fixed test itself (`every_byte_of_a_span_maps_back_to_it`).
- **Lesson / interview angle:** metadata has a lifetime too. Once a span is released, its descriptor is reused for whatever comes next, so a pointer to the descriptor is as dangerous as a pointer to freed memory. The same rule applies inside the allocator: never touch `s` after `retire_span(h, s)`.

### B-003 — Fuzzer oracle checked bytes it had never written (would report false failures)
- **Date found / fixed:** 2026-10-09 / 2026-10-09 (found in self-review, before the first run)
- **Phase / component:** Phase 1 / `tests/fuzz/fuzz_shadow.cpp`, `do_realloc`
- **Symptom:** none observed; it would have reported "contents changed" for correct reallocs of blocks above 4 KiB.
- **Root cause:** for blocks above 4 KiB the fuzzer writes only *sample* positions (first 256 bytes, last 256, one byte per 64 KiB), and the positions depend on the block's size. After `realloc`, the first version verified the prefix using the *new* size's sample positions. The tail samples of the new size fall on bytes the old block never wrote.
- **Fix:**
  ```cpp
  // before
  Live moved{q, size, b.tag};
  verify(moved, b.size < size ? b.size : size);
  // after: check the old block's sample positions (the bytes we wrote), at the new address
  verify(Live{q, b.size, b.tag}, b.size < size ? b.size : size);
  ```
- **Regression test:** `fuzz_shadow` (passes 4 × 10M ops with correct code; mutation M2 shows it still catches a real realloc bug).
- **Lesson / interview angle:** the test oracle is code too, and can be wrong in both directions. A false alarm wastes hours hunting a bug that isn't there. Rule: verify exactly what you wrote.

### B-004 — `test_alloc` depended on state left by earlier tests
- **Date found / fixed:** 2026-10-09 / 2026-10-09
- **Phase / component:** Phase 1 / `tests/unit/test_alloc.cpp`, `empty_spans_and_segments_are_given_back`
- **Symptom:** under mutation M4 the check `mapped >= 5` failed, even though M4 has nothing to do with how many segments a 20 MB allocation needs.
- **How we found the cause:** the check counted segments **newly mapped during this test**. If earlier tests left segments with free pages, this test fills those first and maps fewer new ones. M4 changed how much earlier tests left behind, which changed the count. The check passed with the real code only by luck of test order.
- **Fix:** measure live counts relative to the start of the test.
  ```cpp
  // before
  const std::uint64_t mapped = stats().segments_mapped - before.segments_mapped;
  CHECK(mapped >= 5);
  // after
  auto live_segments = [] { return stats().segments_mapped - stats().segments_unmapped; };
  const std::uint64_t segments_before = live_segments();
  ...
  CHECK(live_segments() >= 5);                     // while 20 MB is live
  ...
  CHECK(live_segments() <= segments_before + 1);   // after freeing it all
  ```
- **Regression test:** the fixed test, plus `scripts/mutation_check.sh`, where M4 now fails only the check it should.
- **Lesson / interview angle:** tests that share a process share the allocator's state. Assert on *changes* relative to a baseline, not on absolute counters, or the test passes and fails depending on what ran before it.

---

## Part 3 — Phase 1 study guide

Explain each answer out loud, with the code open. Function names are given instead of line numbers, because line numbers drift.

1. **How does `free(p)` know the block's size without a header?** `span_of(p)` in `src/segment.h`: mask off the low 22 bits to find the segment, shift by 16 to find the page, then read `pages[i].head`. The span holds `block_size`. (D-005)
2. **Trace `malloc(40)` on the fast path.** `heap_malloc` in `src/heap.h`: `size_to_class(40)` = class 2 (48 B), `current[2]`, pop `free`, `++used`. No syscall, no search. Phase 1 adds the lock in `malloc_impl`.
3. **What happens on the very first `malloc` in the process?** `current[cls]` is `&g_empty_span`, whose `free` is null, so the call takes the slow path (`heap_malloc_small_slow`). That calls `new_small_span` → `alloc_pages` (no segments yet) → `segment_create` → `os::reserve_aligned` → `mmap`. Why does this work before `main()`? `g_heap` and `g_empty_span` are `constinit`. (D-013)
4. **Why do frees go to `local_free` and not straight onto `free`?** (D-009) Then say what moves them back: slow-path step 1.
5. **Why doesn't a new span write a `next` pointer into all 4,096 of its 16-byte blocks?** Writing them would touch every page and make it resident (RSS). `extend_free_list` carves about one OS page at a time. (D-011)
6. **When does a span go back to its segment, and why is the current span kept?** `free_small` → `retire_span`. (D-024, mutation M4)
7. **How are huge blocks freed, and why must memalign stay under 2 MiB?** `segment_create_huge`, and `heap_free` case `kHuge`. (D-022)
8. **Prove that `memalign(64, 70)` returns a 64-byte-aligned block without an interior pointer.** (D-022 worked example)
9. **Why does `libarena.so` link with `-nostdlib++`? What would happen if `os::fatal` used `fprintf`?** It might allocate while we hold the lock: a deadlock. (D-004, D-021, D-026)
10. **How does the fuzzer know a block was handed out twice?** `shadow_insert`: the std::map lives in glibc's heap and is ordered by address, so a new block overlapping a neighbour is visible at once. (B-003 for what the oracle must *not* do)
11. **What is wrong with Phase 1 for a multithreaded program, and what fixes it?** One lock serialises every thread. Phase 2 gives each thread its own heap, and remote frees go through `thread_free` (D-010).

---

## Appendix — Bug-entry template

```markdown
### B-0xx — <one-line description>
- **Date found / fixed:** YYYY-MM-DD / YYYY-MM-DD
- **Phase / component:** e.g. Phase 2 / span.cpp remote free
- **Symptom:** what we saw (crash, wrong output, TSan report). Paste the key error lines.
- **How to reproduce:** the exact command or test.
- **How we found the cause:** the steps taken (gdb, ASan report, adding a CHECK, bisect …), including wrong guesses.
- **Root cause:** the real reason, in plain words.
- **Fix:** what changed and why it is correct.
  ```cpp
  // before
  ...
  // after
  ...
  ```
- **Regression test:** file and test name.
- **Lesson / interview angle:** the general principle this bug teaches.
- **Commit:** <hash>
```
