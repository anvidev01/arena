# Arena — Work Log

Chronological record of every work session: what was done, what was decided, what broke, and what comes next. Newest entry at the bottom.

- *Why* things were decided → [docs/NOTEBOOK.md](docs/NOTEBOOK.md) (D-xxx)
- Bug write-ups → [docs/NOTEBOOK.md](docs/NOTEBOOK.md) (B-xxx)
- Plan and status → [docs/ROADMAP.md](docs/ROADMAP.md)

Entry format:
```
## YYYY-MM-DD — Session N — <short title>
**Phase:** …
**Done:** …
**Decisions:** D-xxx …
**Bugs:** B-xxx …
**Commands / results worth remembering:** …
**Next:** …
```

---

## 2026-10-06 — Session 1 — Architecture and roadmap

**Phase:** 0 (Setup)

**Done:**
- Created the project at `~/Documents/arena` and ran `git init` (branch `main`, nothing committed yet).
- Wrote [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): 6 layers (OS → segment → span → thread heap → core API → shim), size classes, the life of a pointer, `malloc`-specific hazards, debug mode, observability.
- Wrote [docs/ROADMAP.md](docs/ROADMAP.md): Phases 0–4 plus a buffer week, 2026-10-06 → 2026-11-08, each with a "done when" check and interview concepts.
- Wrote [docs/NOTEBOOK.md](docs/NOTEBOOK.md) with decisions D-001 to D-018 and the bug-journal template.
- Wrote [CLAUDE.md](CLAUDE.md) with project rules and the documentation workflow.

**Decisions:** D-001 to D-018 (all from initial design; D-018 still *Proposed*).

**Bugs:** none.

**Environment check:**
- Machine: Apple M4, 10 cores, 16 GB RAM, macOS 15.7.7 (arm64).
- Installed: Docker CLI, cmake (Homebrew), git, Apple clang 17.
- Docker daemon **was not running** (`Cannot connect to the Docker daemon`). Start Docker Desktop before Phase 0.2.

**Next:**
1. Start Docker Desktop; write `docker/Dockerfile` (task 0.2).
2. `CMakeLists.txt` with `arena_core` + `arena` targets and the Release/Debug/TSan/ASan build types (task 0.3).
3. Test runner + first `ctest` (task 0.4), then the first commit (task 0.5).
4. Study: `mmap`, demand paging, RSS vs VSZ (before Phase 1).

---

## 2026-10-06 — Session 2 — Dev environment, OS layer, size classes

**Phase:** 0 (Setup) → started 1

**Done:**
- Started Docker Desktop (Linux VM: kernel 6.10.14-linuxkit, aarch64, 4 KiB pages, 10 CPUs).
- `docker/Dockerfile`: Ubuntu 24.04, clang 18.1.3, glibc 2.39, cmake 3.28, gdb, valgrind, jemalloc + mimalloc 2.1 (for later comparison), redis.
- `scripts/dev.sh` (run anything in the container) and `scripts/check.sh` (build and test release/debug/asan/tsan).
- `CMakeLists.txt`: one object library compiled with `-fno-builtin -fno-exceptions -fno-rtti -fvisibility=hidden -Werror`, packaged as `libarena_core.a` and `libarena.so`.
- `tests/check.h`: test runner with `TEST`, `CHECK`, `CHECK_EQ`.
- **Task 1.1** `src/os.{h,cpp}`: `page_size`, `reserve_aligned` (try once, then over-map and trim), `release`, `decommit`. Tests: 32 segments in a row all 4 MiB-aligned, zeroed, writable at both ends, non-overlapping; a huge size; decommit gives zero pages; bad arguments rejected.
- **Task 1.2** `src/size_class.h`: 40 classes, `class_size`, `size_to_class` (arithmetic), `class_span_pages`. Tests check every size from 1 to 32,768.
- Docs: `CLAUDE.md` build section, `ARCHITECTURE.md` lookup text, roadmap ticks.

**Decisions:** D-019 (arithmetic size → class lookup, supersedes the table idea in D-008), D-020 (container scripts; sanitizers as a CMake option).

**Bugs:** B-001: the ASan link failed because `--no-install-recommends` dropped `libclang-rt-18-dev`. Fixed in the Dockerfile.

**Commands / results worth remembering:**
- `scripts/dev.sh scripts/check.sh all` → 3/3 tests pass in all four configurations.
- Mutation check (to prove the tests can fail): changing the quarter mask in `size_to_class` from `& 3` to `& 1`
  - is caught at **compile time** by `static_assert(size_to_class(kMaxSmallSize) == kNumClasses - 1)`;
  - with that assert removed, `test_size_class` reports **32,640 failed checks** in 2 of its 4 tests.

**Next:**
1. First commit (task 0.5), when Anvi says so.
2. Task 1.3: segment header and page table; pointer → segment → page → span.
3. Study: `src/os.cpp` (why `size + alignment - page` is enough) and `size_to_class` (work an example by hand, as in D-019).

---

## 2026-10-09 — Session 3 — Phase 1 complete: a working allocator

**Phase:** 1 (tasks 1.3–1.8) → ✅ complete

**Done:**
- **1.3 Segments** (`src/segment.{h,cpp}`): header in page 0 with a 64-bit free-page bitmap and 64 `Span` descriptors; first-fit page runs; `segment_of` / `span_of` pointer math; huge segments.
- **1.4 Spans** (`src/span.h`): `free` + `local_free` lists, `used`/`capacity`, lazy bump fill of about one OS page per extension.
- **1.5 Heap** (`src/heap.{h,cpp}`): single `constinit` global heap, `g_empty_span` sentinel, per-class span queue, slow path, span retirement, segment release, large (≤ 1 MiB) and huge paths, `memalign`.
- **1.6 Core API** (`src/alloc.cpp`, `src/api.h`): `arena_malloc/free/calloc/realloc/memalign/usable_size` behind a global spin lock.
- **1.7 Fuzzer** (`tests/fuzz/fuzz_shadow.cpp`): shadow map, overlap/alignment/usable-size/zero/content checks, leak check at the end.
- **1.8 Shim** (`src/shim.cpp`): 11 glibc names with `errno` handling; `scripts/preload_smoke.sh` (8 checks) added to ctest.
- New unit tests: `test_segment` (4 tests), `test_alloc` (11 tests).
- `scripts/mutation_check.sh`: 6 planted bugs, all caught.
- Docs: ARCHITECTURE (L1–L5 as built, new §4a on alignment), NOTEBOOK (D-021–D-029, B-002–B-004, Phase 1 study guide), ROADMAP (Phase 1 ✅), CLAUDE.md layout and commands.

**Decisions:** D-021 global lock (temporary) · D-022 aligned allocation · D-023 page 0 header and first-fit · D-024 retirement/release policy · D-025 realloc rules · D-026 hidden impl and `-nostdlib++ -z defs` · D-027 invalid-free check and Debug asserts · D-028 aligned family moved to Phase 1 · D-029 mutation testing.

**Bugs:**
- B-002: test read a span descriptor after it was wiped (found in review).
- B-003: fuzzer realloc check verified bytes it never wrote (found in review).
- B-004: `test_alloc` depended on earlier tests' state (found by mutation M4).
- Not logged as bugs: two compile errors on the first build (missing `<cstdint>` in `shim.cpp` for `SIZE_MAX`; a `printf` format mismatch in the fuzzer). The compiler caught both.

**Commands / results worth remembering:**
- `scripts/dev.sh scripts/check.sh all` → 7/7 (release, debug), 6/6 (asan, tsan; `preload_smoke` is skipped under sanitizers).
- `build/asan/fuzz_shadow 10000000 1` → PASS in 61 s. Counters: 98 segments mapped / 78 unmapped, 1,368 spans / 1,328 retired (40 left = one current span per class), 137,797 large, 15,202 huge, 417,911 slow-path hits.
- `ldd build/release/libarena.so` → only libc, libm, libgcc_s. `nm -D --defined-only` → 18 exports (11 glibc names + 7 `arena_*`).
- Mutation check: `scripts/dev.sh scripts/mutation_check.sh` → 6 caught, 0 missed. It also found:
  - M4 ("release the current span") is *not* a correctness bug; added a ping-pong test for the performance rule.
  - M5 made the release build *hang*; added a Debug invariant assert so it aborts with a message instead.
- No timing numbers recorded: Docker on the Mac is not a benchmark machine (D-017).

**Next:**
1. Commit Phase 0 + 1 (waiting for Anvi's go-ahead).
2. Phase 2: per-thread heaps with `initial-exec` TLS, bootstrap memory for heaps, `thread_free` remote frees, thread exit, then remove the global lock (D-021).
3. Study: Part 3 of NOTEBOOK.md (11 questions), then the mimalloc paper sections 2–3 before Phase 2.

---

## 2026-10-12 — Session 4 — First commit, pushed to GitHub

**Done:**
- Commit `664942e` "Arena Phase 0 + 1": 33 files, all of Phase 0 and Phase 1. `build/` is excluded by `.gitignore`. Checked before committing: no personal data or secrets in the tracked files.
- Remote: `origin` = `git@github.com:anvidev01/arena.git` (SSH; the `gh` CLI is not logged in). `main` pushed and tracking `origin/main`.
- Roadmap task 0.5 ticked.

**Next:** Phase 2 (per-thread heaps, remote frees, remove the global lock).
