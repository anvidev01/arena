// Proves the build, link and test pipeline works end to end.
#include <cstring>

#include "arena/arena.h"
#include "check.h"

TEST(version_is_set) { CHECK(std::strcmp(arena_version(), "0.1.0") == 0); }

int main() { return arena_test::run_all(); }
