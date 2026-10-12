// Shadow-map fuzzer (task 1.7).
//
// Runs millions of random malloc / calloc / realloc / memalign / free calls against Arena,
// and keeps an independent record (the "shadow map") of every live block in a std::map that
// lives in the *system* allocator (D-003). After every operation it checks:
//   1. the new block does not overlap any live block (using the full usable size),
//   2. it is 16-byte aligned (and `align`-aligned for memalign),
//   3. usable_size >= requested size,
//   4. calloc memory is zero,
//   5. contents survive: each block is filled with a pattern derived from its tag, and the
//      pattern is verified on free and (for the kept prefix) on realloc.
// At the end it frees everything and checks the heap gave back its large/huge blocks.
//
// Usage: fuzz_shadow [ops=1000000] [seed=1]
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "arena/arena.h"
#include "heap.h"

namespace {

struct Rng {  // splitmix64: tiny, fast, and the run is reproducible from the seed
  std::uint64_t s;
  std::uint64_t next() {
    std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
};

struct Live {
  std::uint8_t* p;
  std::size_t size;
  std::uint32_t tag;
};

constexpr std::size_t kMaxLive = 20000;
constexpr std::size_t kMaxLiveBytes = 256u << 20;  // keep RSS bounded

std::vector<Live> live;
std::map<std::uintptr_t, std::size_t> shadow;  // block start -> usable size
std::size_t live_bytes = 0;
std::uint64_t failures = 0;
std::uint64_t op_index = 0;

void fail(const char* what, const void* p, std::size_t size) {
  if (failures++ < 20) {
    std::fprintf(stderr, "op %" PRIu64 ": %s (p=%p size=%zu)\n", op_index, what, p, size);
  }
}

// Positions we fill and verify: every byte of small blocks; head, tail and one byte per
// 64 KiB of big ones (so the test doesn't touch every page of a 4 MiB block).
template <typename F>
void for_each_sample(std::size_t size, F f) {
  if (size <= 4096) {
    for (std::size_t i = 0; i < size; ++i) f(i);
    return;
  }
  for (std::size_t i = 0; i < 256; ++i) f(i);
  for (std::size_t i = 256; i < size - 256; i += 65536) f(i);
  for (std::size_t i = size - 256; i < size; ++i) f(i);
}

std::uint8_t pattern(std::uint32_t tag, std::size_t i) {
  return static_cast<std::uint8_t>(tag * 131u + i * 31u + (i >> 8));
}

void fill(const Live& b) {
  for_each_sample(b.size, [&](std::size_t i) { b.p[i] = pattern(b.tag, i); });
}

void verify(const Live& b, std::size_t limit) {
  bool ok = true;
  for_each_sample(b.size, [&](std::size_t i) {
    if (i < limit && b.p[i] != pattern(b.tag, i)) ok = false;
  });
  if (!ok) fail("contents changed", b.p, b.size);
}

void shadow_insert(std::uint8_t* p, std::size_t size, std::size_t align) {
  const auto a = reinterpret_cast<std::uintptr_t>(p);
  const std::size_t usable = arena_usable_size(p);
  if (a % 16 != 0) fail("not 16-byte aligned", p, size);
  if (align > 16 && a % align != 0) fail("memalign: wrong alignment", p, size);
  if (usable < size) fail("usable_size < size", p, size);

  auto next = shadow.lower_bound(a);
  if (next != shadow.end() && a + usable > next->first) fail("overlaps the next live block", p, size);
  if (next != shadow.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second > a) fail("overlaps the previous live block", p, size);
  }
  shadow[a] = usable;
}

void shadow_erase(const std::uint8_t* p) {
  if (shadow.erase(reinterpret_cast<std::uintptr_t>(p)) != 1) fail("freed block not in shadow", p, 0);
}

std::size_t random_size(Rng& r) {
  const std::uint64_t k = r.below(100);
  if (k < 60) return r.below(129);                 // tiny, includes 0
  if (k < 85) return 129 + r.below(4096 - 128);    // small
  if (k < 95) return 4097 + r.below(32768 - 4096); // medium small classes
  if (k < 99) return 32769 + r.below(1 << 20);     // large (and the large/huge border)
  return (1 << 20) + r.below(3 << 20);             // huge, 1-4 MiB
}

void do_alloc(Rng& r, std::uint32_t tag) {
  const std::size_t size = random_size(r);
  if (live.size() >= kMaxLive || live_bytes + size > kMaxLiveBytes) return;
  const std::uint64_t kind = r.below(100);
  std::size_t align = 0;
  std::uint8_t* p;
  if (kind < 80) {
    p = static_cast<std::uint8_t*>(arena_malloc(size));
  } else if (kind < 90) {
    p = static_cast<std::uint8_t*>(arena_calloc(1, size));
    if (p) {
      bool zero = true;
      for_each_sample(size, [&](std::size_t i) { zero = zero && p[i] == 0; });
      if (!zero) fail("calloc memory not zero", p, size);
    }
  } else {
    align = std::size_t{1} << (r.below(100) < 95 ? 4 + r.below(9) : 13 + r.below(9));  // 16 B .. 2 MiB
    p = static_cast<std::uint8_t*>(arena_memalign(align, size));
  }
  if (p == nullptr) return fail("allocation returned NULL", nullptr, size);
  shadow_insert(p, size, align);
  live.push_back({p, size, tag});
  live_bytes += size;
  fill(live.back());
}

void do_free(Rng& r) {
  if (live.empty()) return;
  const std::size_t i = r.below(live.size());
  verify(live[i], live[i].size);
  shadow_erase(live[i].p);
  live_bytes -= live[i].size;
  arena_free(live[i].p);
  live[i] = live.back();
  live.pop_back();
}

void do_realloc(Rng& r, std::uint32_t tag) {
  if (live.empty()) return;
  const std::size_t i = r.below(live.size());
  Live& b = live[i];
  const std::size_t size = random_size(r);
  if (live_bytes - b.size + size > kMaxLiveBytes) return;
  verify(b, b.size);
  shadow_erase(b.p);
  auto* q = static_cast<std::uint8_t*>(arena_realloc(b.p, size));
  if (q == nullptr && size != 0) {
    fail("realloc returned NULL", b.p, size);
    shadow_insert(b.p, b.size, 0);  // old block is still live
    return;
  }
  if (size == 0) {  // realloc(p, 0) freed it (glibc semantics)
    live_bytes -= b.size;
    live[i] = live.back();
    live.pop_back();
    return;
  }
  // The kept prefix must be unchanged. Check the old block's sample positions (those are
  // the bytes we actually wrote), now at the new address, up to the smaller size.
  verify(Live{q, b.size, b.tag}, b.size < size ? b.size : size);
  shadow_insert(q, size, 0);
  live_bytes = live_bytes - b.size + size;
  b = {q, size, tag};
  fill(b);
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t ops = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
  const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
  Rng rng{seed};
  std::size_t peak_live = 0;

  for (op_index = 0; op_index < ops; ++op_index) {
    const std::uint32_t tag = static_cast<std::uint32_t>(op_index);
    const std::uint64_t k = rng.below(100);
    // Phases bias toward growing or shrinking, so the live set swings between empty and
    // full; that exercises span retirement and segment unmapping, not just steady state.
    const bool growing = (op_index / 50000) % 2 == 0;
    if (k < (growing ? 55u : 35u)) do_alloc(rng, tag);
    else if (k < 85) do_free(rng);
    else do_realloc(rng, tag);
    if (live.size() > peak_live) peak_live = live.size();
  }

  while (!live.empty()) do_free(rng);
  const arena::HeapStats& st = arena::global_heap()->stats;
  if (st.large_allocs != st.large_frees) fail("large blocks leaked", nullptr, st.large_allocs - st.large_frees);
  if (st.huge_allocs != st.huge_frees) fail("huge blocks leaked", nullptr, st.huge_allocs - st.huge_frees);
  // Every non-current small span must be retired: at most one live span per size class.
  if (st.spans_created - st.spans_retired > arena::kNumClasses) fail("small spans leaked", nullptr, 0);

  std::printf("fuzz_shadow: %" PRIu64 " ops, seed %" PRIu64 ", peak live %zu blocks\n", ops, seed,
              peak_live);
  std::printf("  segments mapped %" PRIu64 " / unmapped %" PRIu64 ", spans %" PRIu64 " / retired %" PRIu64
              ", large %" PRIu64 ", huge %" PRIu64 ", slow path %" PRIu64 "\n",
              st.segments_mapped, st.segments_unmapped, st.spans_created, st.spans_retired,
              st.large_allocs, st.huge_allocs, st.slow_path);
  std::printf("  %s (%" PRIu64 " failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
  return failures == 0 ? 0 : 1;
}
