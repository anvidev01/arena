# Arena — Roadmap

**Start:** Tue 2026-10-06 · **Target finish:** Sun 2026-11-08 (4 weeks + 1 buffer week) · **Pace:** about 12–15 hrs/week alongside Codeforces

Each phase ends with a **"done when"** check. Do not move to the next phase until every box is ticked. A half-working allocator corrupts memory in ways that are much harder to debug later.

Status key: ⬜ not started · 🟨 in progress · ✅ done

Design reference: [ARCHITECTURE.md](ARCHITECTURE.md) · Reasons: [NOTEBOOK.md](NOTEBOOK.md) · History: [../LOG.md](../LOG.md)

---

## Phase 0 — Setup (Tue 6 – Wed 7 Oct) 🟨 (only 0.5, the first commit, is left)

| # | Task | Status |
|---|---|---|
| 0.1 | Architecture, roadmap, notebook, log and CLAUDE.md written | ✅ |
| 0.2 | Start Docker Desktop; `docker/Dockerfile` for Ubuntu 24.04 with clang, gcc, cmake, gdb, valgrind, redis, jemalloc and mimalloc libraries. `perf` left out: it needs host kernel access, which Docker on the Mac lacks; install it on the Phase 4 benchmark machine | ✅ |
| 0.3 | `CMakeLists.txt` with two targets (`arena_core` static, `arena` shared). Flags: `-O2 -fno-builtin -fno-exceptions -fno-rtti -fvisibility=hidden`. Build types: Release / Debug / TSan / ASan | ✅ |
| 0.4 | Test framework: a tiny assert-based runner (no GoogleTest, see D-016) plus a `ctest` target | ✅ |
| 0.5 | First commit with the docs and an empty skeleton that compiles | ⬜ |

**Done when:** `docker run … cmake --build build && ctest` passes on an empty test inside the container.

**Study before Phase 1:** virtual vs physical memory; what `mmap(MAP_PRIVATE|MAP_ANONYMOUS)` does; demand paging and first-touch; RSS vs VSZ.

---

## Phase 1 — Single-threaded allocator (Week 1: Thu 8 – Wed 14 Oct) ✅ (finished Fri 9 Oct)

| # | Task |
|---|---|
| 1.1 ✅ | **L0** `os.cpp`: aligned reserve, release, decommit, page size. Unit test: is the returned address 4 MiB-aligned? |
| 1.2 ✅ | **Size classes**: the class table and `size_to_class`. Unit test: for every size from 1 to 32 KiB, the class is ≥ size, waste is < 25%, and the result is monotonic |
| 1.3 ✅ | **L1** segments: header, page table, pointer → segment → page → span math |
| 1.4 ✅ | **L2** spans: lazy fill, `free` / `local_free` lists, `used` count |
| 1.5 ✅ | **L3** a single global heap (no threads yet), the span queue per class, large and huge paths |
| 1.6 ✅ | **L4** `arena_malloc` / `arena_free` / `arena_calloc` / `arena_realloc` |
| 1.7 ✅ | **Shadow-map fuzzer**: millions of random `malloc`/`free`/`realloc` calls. A `std::map` from the system allocator records every live range. Check: no two live blocks overlap, every block is aligned, and the contents survive (fill on allocate, verify on free) |
| 1.8 ✅ | **L5** shim plus first `LD_PRELOAD` runs: `ls`, `echo`, `python3 -c "print(1)"` |

**Done when:** the fuzzer runs 10M operations clean under ASan (core API); `python3 -c "import json; print(json.dumps(list(range(10**5)))[:20])"` works under `LD_PRELOAD`.

**Result (2026-10-09):** ✅ both met.
- `build/asan/fuzz_shadow 10000000 1` → PASS, 0 failures (61 s). Seeds 2, 3 and 4 also ran 10M ops each in release → PASS.
- `preload_smoke.sh`: Arena confirmed active; `echo`, `ls`, `sort`, `redis-server --version`, the roadmap's Python command, a heavier Python workload and a `gcc` compile-and-run all produce identical output with and without Arena.
- All 7 tests pass in release, debug, ASan and TSan; mutation check catches 6 of 6 planted bugs (D-029).
- Ahead of plan: the aligned family (task 3.1) was built now (D-028). Still single-heap with a global lock (D-021), so Phase 2 starts by removing it.

**Interview concepts:** `brk` vs `mmap`; internal vs external fragmentation; why size classes; why the free list can be intrusive; how `free` knows a block's size without a header.

---

## Phase 2 — Thread heaps and remote frees (Week 2: Thu 15 – Wed 21 Oct) ⬜

| # | Task |
|---|---|
| 2.1 | `thread_local` heap with `initial-exec` TLS and the `empty_heap` sentinel |
| 2.2 | Heaps allocated from Arena's own bootstrap memory (no `malloc` recursion) |
| 2.3 | Ownership check in `free`; `thread_free` CAS push; `exchange(nullptr)` collection in the slow path |
| 2.4 | Thread exit: `pthread_key` destructor, abandoned spans, adoption by other threads |
| 2.5 | **Stress tests under TSan** (core API): (a) N threads each allocate and free locally; (b) producer–consumer, where thread A allocates and thread B frees; (c) threads that exit while their blocks are still alive elsewhere |
| 2.6 | Cache-line alignment of per-thread state; check for false sharing |

**Done when:** all stress tests are TSan-clean for 1, 2, 4 and 8 threads; the fuzzer still passes; a multithreaded program (`python3 -m test test_threading` or a pthreads test) runs under `LD_PRELOAD`.

**Interview concepts:** the ABA problem and why taking the whole list avoids it; memory ordering (`release` on push, `acquire` on take); false sharing; why jemalloc uses arenas and tcmalloc uses per-CPU caches.

---

## Phase 3 — Full API and real programs (Week 3: Thu 22 – Wed 28 Oct) ⬜

| # | Task |
|---|---|
| 3.1 🟨 | Implemented early in Phase 1 (D-028); edge-case tests remain. `posix_memalign` / `aligned_alloc` / `memalign` / `valloc` / `pvalloc`, `malloc_usable_size`, `reallocarray` |
| 3.2 | POSIX edge cases: `calloc` overflow → `ENOMEM`, `realloc(p, 0)`, `realloc(NULL, n)`, `malloc(0)`, setting `errno` |
| 3.3 | In-place `realloc` when the new size fits in the same class |
| 3.4 | `fork()` safety through `pthread_atfork` |
| 3.5 | **Debug mode**: poisoning, encoded free lists, double-free detection, guard pages |
| 3.6 | `ARENA_STATS=1` exit report |
| 3.7 | `scripts/real_programs.sh`: under `LD_PRELOAD`, (a) build a real C project (e.g. Lua or SQLite) with `gcc -j`; (b) run `redis-server` + `redis-benchmark`; (c) run a Python test-suite subset; (d) run `git` operations on a large repository |

**Done when:** every program in `real_programs.sh` produces the same output with and without Arena. Any bug found gets a NOTEBOOK B-entry and a regression test.

**Interview concepts:** what `fork` does to threads and locks; why `calloc` can skip zeroing fresh `mmap` memory; alignment rules; undefined behaviour of foreign frees.

---

## Phase 4 — Benchmarks and write-up (Week 4: Thu 29 Oct – Wed 4 Nov) ⬜

| # | Task |
|---|---|
| 4.1 | Get mimalloc-bench running with glibc, jemalloc and mimalloc; add Arena as an allocator entry |
| 4.2 | `bench/latency`: per-call `malloc`/`free` latency histogram, p50/p99/p99.9 (timing method in D-018) |
| 4.3 | `bench/scaling`: throughput from 1 to N threads |
| 4.4 | Peak RSS and fragmentation for each workload |
| 4.5 | Experiments: huge pages on or off; decommit delay; segment size |
| 4.6 | **Per-workload analysis**: for every benchmark, *why* Arena wins or loses (fragmentation, remote frees, false sharing), supported by Arena's stats counters. This is the part that makes the project stand out |
| 4.7 | README with results, graphs and the resume bullet with real numbers |
| 4.8 | Optional: run Vortex with Arena vs glibc |

**Done when:** `docs/results/` holds the raw data, graphs and a write-up; the README's resume bullet has every placeholder replaced with real numbers.

**Where the numbers come from:** final numbers are measured on a dedicated x86-64 Linux machine, not in Docker on the Mac (D-017).

---

## Buffer week (Thu 5 – Sun 8 Nov)
For whatever slipped. If nothing slipped, make the write-up and graphs better.

---

## Risks

| Risk | Plan |
|---|---|
| A memory-corruption bug that takes days to find | Fuzzer and debug mode come *before* real programs; commit small steps so `git bisect` works |
| Docker timing on the Mac is noisy | Use it for correctness only; measure on dedicated hardware (D-017) |
| Phase 2 concurrency bugs | TSan from day one of Phase 2; keep the remote-free design simple (whole-list exchange) |
| Falling behind schedule | Cut huge pages (4.5) and the debug-mode extras first. Never cut the fuzzer, the TSan tests or the per-workload analysis |
