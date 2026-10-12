// Internal invariant checks. On in Debug builds and with -DARENA_DEBUG=ON; compiled out in
// release. They report with write(2) and abort, because printf may call malloc.
#pragma once

#include "os.h"

#if defined(ARENA_DEBUG) || !defined(NDEBUG)
#define ARENA_ASSERT(cond, msg)                                  \
  do {                                                           \
    if (!(cond)) ::arena::os::fatal("arena: " msg "\n");         \
  } while (0)
#else
#define ARENA_ASSERT(cond, msg) \
  do {                          \
  } while (0)
#endif
