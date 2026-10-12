#!/usr/bin/env bash
# Configure, build and test one or all configurations. Run inside the container:
#   scripts/dev.sh scripts/check.sh            -> release
#   scripts/dev.sh scripts/check.sh asan       -> one config
#   scripts/dev.sh scripts/check.sh all        -> release, debug, asan, tsan
set -euo pipefail

run_config() {
  local name=$1; shift
  local dir=build/$name
  echo "=== $name ==="
  cmake -S . -B "$dir" -G Ninja "$@" >/dev/null
  cmake --build "$dir"
  ctest --test-dir "$dir" --output-on-failure
}

config() {
  case $1 in
    release) run_config release -DCMAKE_BUILD_TYPE=RelWithDebInfo ;;
    debug)   run_config debug   -DCMAKE_BUILD_TYPE=Debug -DARENA_DEBUG=ON ;;
    asan)    run_config asan    -DCMAKE_BUILD_TYPE=Debug -DARENA_SANITIZE=address ;;
    tsan)    run_config tsan    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DARENA_SANITIZE=thread ;;
    *) echo "unknown config: $1" >&2; exit 2 ;;
  esac
}

if [ "${1:-release}" = all ]; then
  for c in release debug asan tsan; do config "$c"; done
else
  config "${1:-release}"
fi
