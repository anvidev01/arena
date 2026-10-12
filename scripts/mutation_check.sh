#!/usr/bin/env bash
# Mutation check: plant known bugs, one at a time, in a throwaway copy of the tree and
# confirm the tests catch each one. Proves the tests can fail, not just that they pass.
# Run inside the container:   scripts/dev.sh scripts/mutation_check.sh
set -u

SRC=$(pwd)
WORK=/tmp/arena-mutation
caught=0
missed=0

# mutate <name> <build type> <file> <exact text> <replacement> <command that must FAIL>
mutate() {
  local name=$1 type=$2 file=$3 from=$4 to=$5 check=$6
  rm -rf "$WORK" && cp -r "$SRC" "$WORK" && rm -rf "$WORK/build" && cd "$WORK" || exit 2
  python3 - "$file" "$from" "$to" <<'PY' || { echo "!! $name: text not found"; missed=$((missed+1)); cd "$SRC"; return; }
import sys
path, a, b = sys.argv[1:]
s = open(path).read()
if a not in s: sys.exit(1)
open(path, 'w').write(s.replace(a, b, 1))
PY
  cmake -S . -B b -G Ninja -DCMAKE_BUILD_TYPE="$type" >/dev/null
  if ! cmake --build b >/tmp/mutation.out 2>&1; then  # caught at compile time
    echo "caught  $name"; grep -m1 "error:" /tmp/mutation.out | sed 's|^.*error: |          compile error: |'
    caught=$((caught + 1)); cd "$SRC"; return
  fi
  if timeout 120 bash -c "$check" >/tmp/mutation.out 2>&1; then
    echo "MISSED  $name"; missed=$((missed + 1))
  else
    echo "caught  $name"; grep -m1 -E "^op |CHECK failed|^arena:" /tmp/mutation.out | sed 's/^/          /'
    caught=$((caught + 1))
  fi
  cd "$SRC"
}

FUZZ="./b/fuzz_shadow 300000 1"

mutate "M1 bump pointer one block short (block handed out twice)" RelWithDebInfo src/heap.cpp \
  "s->bump = p + n * bs;" "s->bump = p + (n - 1) * bs;" "$FUZZ"
mutate "M2 realloc copies one byte too few" RelWithDebInfo src/alloc.cpp \
  "std::memcpy(q, p, size < usable ? size : usable);" \
  "std::memcpy(q, p, (size < usable ? size : usable) - 1);" "$FUZZ"
mutate "M3 calloc forgets to zero" RelWithDebInfo src/alloc.cpp \
  "if (p != nullptr) std::memset(p, 0, total);" "(void)total;" "$FUZZ"
mutate "M4 empty current span is released (performance rule)" RelWithDebInfo src/heap.cpp \
  "} else if (s->used == 0 && h->current[cls] != s) {" "} else if (s->used == 0) {" "./b/test_alloc"
mutate "M5 queue_remove leaves current dangling (Debug assert)" Debug src/heap.cpp \
  $'  s->in_queue = false;\n  refresh_current(h, cls);' "  s->in_queue = false;" "$FUZZ"
mutate "M6 size_to_class picks the wrong quarter" RelWithDebInfo src/size_class.h \
  "(s >> (top - 2)) & 3" "(s >> (top - 2)) & 1" "true"   # static_assert stops the build

echo "mutation_check: $caught caught, $missed missed"
exit $((missed != 0))
