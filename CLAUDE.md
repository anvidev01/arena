# CLAUDE.md — Arena

Arena is a thread-caching `malloc` replacement for Linux, loaded with `LD_PRELOAD` and benchmarked against glibc, jemalloc and mimalloc. It is a placement project aimed at HFT systems roles. Anvi must be able to **explain every line and every decision in an interview**, so the documentation matters as much as the code.

## Read first, every session
1. [docs/ROADMAP.md](docs/ROADMAP.md): current phase and next unticked task.
2. The last entry in [LOG.md](LOG.md): where the previous session stopped.
3. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the layer you are about to touch.
4. Relevant D-entries in [docs/NOTEBOOK.md](docs/NOTEBOOK.md). Do not quietly contradict an accepted decision; propose a new D-entry that supersedes it.

## Documentation workflow (required, not optional)
| When | Update |
|---|---|
| End of every session | Append a session entry to `LOG.md` (format at top of the file). Tick tasks in `ROADMAP.md`. |
| Any design choice with a real alternative | New `D-0xx` in `NOTEBOOK.md`: context, decision, alternatives, cost, interview line. Add it to the index table. Never renumber; superseded entries stay, marked *Superseded by D-0yy*. |
| Any bug (crash, wrong result, sanitizer report, failed test that wasn't expected to fail) | New `B-0xx` in `NOTEBOOK.md` using the template: symptom, repro, how the cause was found (including wrong guesses), root cause, before/after code, regression test, lesson. A regression test is mandatory. |
| Architecture changes (sizes, layers, data structures) | Update `ARCHITECTURE.md` in the same change. |
| Commands, flags or layout change | Update the "Build and test" section below. |

Dates are absolute (`YYYY-MM-DD`). Explain things for someone who is learning: say *why*, not just *what*.

## Code rules (from D-004 and the hazards table in ARCHITECTURE.md §6)
- **Nothing inside Arena may call `malloc`, directly or indirectly.** No `std::vector`, `std::string`, `std::map`, `iostream`, `printf`/`snprintf`, `dlsym`, exceptions. Output goes through `write(2)` and Arena's own formatter.
- Compile with `-fno-builtin -fno-exceptions -fno-rtti -fvisibility=hidden`. Only the shim exports symbols.
- The owner-thread fast path has **no locks and no atomics**. Any atomic needs an explicit memory order and a comment saying why that order is enough.
- Only `src/os.cpp` makes system calls.
- Thread-locals use `initial-exec` TLS.
- Comments explain *why* (invariants, ordering, hazards), not what the next line does. Every struct field that is shared between threads says who reads and who writes it.
- Tests call the `arena_*` core API (so TSan and ASan work). `LD_PRELOAD` is only for real-program runs.
- Small, focused commits so `git bisect` works. Commit only when Anvi asks.
- Remote: `origin` = `git@github.com:anvidev01/arena.git` (push over SSH; `gh` is not logged in, so it can't create repos or PRs).

## Environment
- Host: Apple M4 Mac, macOS 15.7 (arm64). Arena does **not** build natively on macOS (D-001).
- Build and test inside the Ubuntu 24.04 Docker container (`docker/Dockerfile`, Phase 0.2). Docker Desktop must be running.
- Benchmark numbers for the write-up come from dedicated x86-64 Linux only (D-017). Never publish timing measured in Docker on the Mac.

## Build and test
Run from the repo root on the Mac (Docker Desktop must be running):
```
scripts/dev.sh                          # interactive shell in the container (repo at /arena)
scripts/dev.sh scripts/check.sh         # build + ctest, release (RelWithDebInfo)
scripts/dev.sh scripts/check.sh asan    # one of: release | debug | asan | tsan
scripts/dev.sh scripts/check.sh all     # all four, run before every commit
docker build -t arena-dev docker/       # after editing the Dockerfile, then rerun `check.sh all` (B-001)
```
- Build dirs: `build/<config>/`. Outputs: `libarena_core.a`, `libarena.so`, `test_*`.
- CMake options: `-DARENA_DEBUG=ON`, `-DARENA_SANITIZE=address|thread` (D-020).
- New unit test: add `tests/unit/test_<name>.cpp` (uses `tests/check.h`), then `arena_test(test_<name>)` in `CMakeLists.txt`.
- New source file: add it to `arena_obj` in `CMakeLists.txt` (`src/shim.cpp` is the only file added to `arena` alone).
- Long fuzz runs by hand: `scripts/dev.sh build/asan/fuzz_shadow 10000000 <seed>` (about 60 s).
- Mutation check: `scripts/dev.sh scripts/mutation_check.sh` (about 2 min).

## Layout
```
CLAUDE.md  LOG.md  CMakeLists.txt
docker/Dockerfile            Ubuntu 24.04, clang 18, glibc 2.39
include/arena/arena.h        core API (arena_*)
src/config.h                 segment / page / alignment constants
src/os.{h,cpp}               L0: mmap, munmap, madvise, yield, fatal (only file with syscalls)
src/size_class.h             40 size classes, lookup, large/huge/align limits
src/span.h                   L2: Block, Span descriptor
src/segment.{h,cpp}          L1: segments, page bitmap, segment_of / span_of
src/heap.{h,cpp}             L3: heap, span queues, malloc fast path (inline) + slow paths
src/api.h  src/alloc.cpp     L4: hidden *_impl functions, global lock (Phase 1), arena_* API
src/shim.cpp                 L5: malloc/free/... exports, libarena.so only
src/check_internal.h         ARENA_ASSERT (Debug / ARENA_DEBUG only)
tests/check.h                test runner (TEST, CHECK, CHECK_EQ)
tests/unit/                  one executable per file
tests/fuzz/fuzz_shadow.cpp   shadow-map fuzzer: fuzz_shadow [ops] [seed]
scripts/dev.sh check.sh      container wrapper, build+test
scripts/preload_smoke.sh     real programs under LD_PRELOAD (a ctest)
scripts/mutation_check.sh    plant known bugs, require the tests to catch them (D-029)
docs/      ARCHITECTURE.md  ROADMAP.md  NOTEBOOK.md  (results/: Phase 4)
```

## Current state (update when it changes)
- Phase 1 complete (2026-10-09). **One global heap behind one spin lock (D-021)**: the lock is temporary, and nothing measured on it means anything for performance.
- After changing allocator code, run `check.sh all`, then `mutation_check.sh`. If a mutation is missed, strengthen the tests before moving on.
