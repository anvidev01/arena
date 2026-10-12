// Minimal test runner (D-016). Usage:
//
//   TEST(name) { CHECK(x > 0); CHECK_EQ(a, b); }
//   int main() { return arena_test::run_all(); }
//
// A failed CHECK reports and continues; the test binary exits non-zero if any check failed.
#pragma once

#include <cstdio>
#include <vector>

namespace arena_test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}
inline int failures = 0;

struct Register {
  Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int run_all() {
  for (const Case& c : registry()) {
    const int before = failures;
    c.fn();
    std::printf("[%s] %s\n", failures == before ? " ok " : "FAIL", c.name);
  }
  std::printf("%zu tests, %d failed checks\n", registry().size(), failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace arena_test

#define TEST(name)                                                   \
  static void test_##name();                                         \
  static ::arena_test::Register reg_##name(#name, &test_##name);     \
  static void test_##name()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++::arena_test::failures;                                                  \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                             \
  do {                                                                             \
    const auto va_ = (a);                                                          \
    const auto vb_ = (b);                                                          \
    if (!(va_ == vb_)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%llu vs %llu)\n",    \
                   __FILE__, __LINE__, #a, #b, (unsigned long long)va_,            \
                   (unsigned long long)vb_);                                       \
      ++::arena_test::failures;                                                    \
    }                                                                              \
  } while (0)
