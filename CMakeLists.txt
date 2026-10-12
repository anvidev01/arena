cmake_minimum_required(VERSION 3.20)
project(arena VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Arena targets Linux only (D-001). Build inside the container: scripts/dev.sh scripts/check.sh")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(ARENA_DEBUG "Enable debug checks: poisoning, double-free detection, guard pages" OFF)
set(ARENA_SANITIZE "" CACHE STRING "Sanitizer for core + tests: '', 'address' or 'thread'")

# Sanitizers instrument Arena's own code too, so TSan sees races inside the allocator (D-003).
if(ARENA_SANITIZE)
  add_compile_options(-fsanitize=${ARENA_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${ARENA_SANITIZE})
endif()

# ---- Allocator code ----------------------------------------------------------
# Flags every allocator source file gets (D-004).
add_library(arena_flags INTERFACE)
target_include_directories(arena_flags INTERFACE include src)
target_compile_options(arena_flags INTERFACE
  -Wall -Wextra -Werror
  -fno-builtin        # stop malloc+memset becoming a calloc call inside calloc (D-004)
  -fno-exceptions -fno-rtti
  -fvisibility=hidden # only ARENA_API / ARENA_EXPORT functions are exported
)
if(ARENA_DEBUG)
  target_compile_definitions(arena_flags INTERFACE ARENA_DEBUG=1)
endif()

# Compiled once as position-independent objects, then packaged twice (D-003):
#   arena_core  (static)  -> tests and benchmarks, arena_* names only
#   arena       (shared)  -> LD_PRELOAD; adds the shim that exports malloc/free/...
add_library(arena_obj OBJECT
  src/alloc.cpp
  src/heap.cpp
  src/os.cpp
  src/segment.cpp
)
target_link_libraries(arena_obj PRIVATE arena_flags)
set_target_properties(arena_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(arena_core STATIC $<TARGET_OBJECTS:arena_obj>)
target_include_directories(arena_core PUBLIC include)

add_library(arena SHARED $<TARGET_OBJECTS:arena_obj> src/shim.cpp)
target_link_libraries(arena PRIVATE arena_flags)
# -nostdlib++: libarena.so must not depend on libstdc++ (which allocates). If any code
# needs it, the link fails here instead of recursing at runtime. -z defs makes every
# undefined symbol a link error. (Sanitizer runtimes leave symbols undefined, so skip it.)
target_link_options(arena PRIVATE -nostdlib++)
if(NOT ARENA_SANITIZE)
  target_link_options(arena PRIVATE -Wl,-z,defs)
endif()

# ---- Tests -------------------------------------------------------------------
# Tests use the system malloc for their own bookkeeping and call Arena via arena_* (D-003).
enable_testing()
function(arena_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE src tests)
  target_link_libraries(${name} PRIVATE arena_core)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

arena_test(test_smoke tests/unit/test_smoke.cpp)
arena_test(test_os tests/unit/test_os.cpp)
arena_test(test_size_class tests/unit/test_size_class.cpp)
arena_test(test_segment tests/unit/test_segment.cpp)
arena_test(test_alloc tests/unit/test_alloc.cpp)

# Shadow-map fuzzer (task 1.7). ctest runs 1M ops; for the 10M-op check run it by hand:
#   build/asan/fuzz_shadow 10000000 <seed>
arena_test(fuzz_shadow tests/fuzz/fuzz_shadow.cpp)
set_tests_properties(fuzz_shadow PROPERTIES TIMEOUT 600)

# Real programs under LD_PRELOAD (task 1.8). Not with sanitizers: they bring their own malloc.
if(NOT ARENA_SANITIZE)
  add_test(NAME preload_smoke
           COMMAND ${CMAKE_SOURCE_DIR}/scripts/preload_smoke.sh $<TARGET_FILE:arena>)
endif()
