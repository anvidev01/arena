#!/usr/bin/env bash
# Task 1.8: run real programs with and without libarena.so preloaded; outputs must match.
#   scripts/preload_smoke.sh build/release/libarena.so
set -uo pipefail

LIB=$(realpath "${1:?usage: preload_smoke.sh path/to/libarena.so}")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0

# run_both <name> <command...>: compare stdout+exit status of the command without/with Arena.
run_both() {
  local name=$1; shift
  "$@" >"$TMP/sys.out" 2>"$TMP/sys.err"; local sys=$?
  LD_PRELOAD="$LIB" "$@" >"$TMP/arena.out" 2>"$TMP/arena.err"; local arena=$?
  if [ $sys -ne $arena ] || ! cmp -s "$TMP/sys.out" "$TMP/arena.out"; then
    echo "FAIL $name (exit $sys vs $arena)"; sed 's/^/    /' "$TMP/arena.err" | head -20
    fails=$((fails + 1))
  else
    echo "ok   $name"
  fi
}

# 1. Proof that Arena is the malloc in use: 40 B rounds up to the 48 B class (glibc says 40).
probe='import ctypes; c=ctypes.CDLL(None); c.malloc.restype=ctypes.c_void_p
c.malloc_usable_size.restype=ctypes.c_size_t; c.malloc_usable_size.argtypes=[ctypes.c_void_p]
print(c.malloc_usable_size(c.malloc(40)))'
got=$(LD_PRELOAD="$LIB" python3 -c "$probe")
if [ "$got" = 48 ]; then echo "ok   arena is active (malloc_usable_size(malloc(40)) = 48)"
else echo "FAIL arena not active: got '$got'"; fails=$((fails + 1)); fi

# 2. Everyday tools.
run_both "echo"        echo hello arena
run_both "ls -la"      ls -la /usr/lib
run_both "sort"        bash -c 'seq 200000 | sort -r | sort -n | tail -3'
run_both "redis --version" bash -c 'redis-server --version | cut -d" " -f1-3'

# 3. Python: the roadmap's "done when" command, then a heavier allocation workload.
run_both "python json (roadmap check)" \
  python3 -c "import json; print(json.dumps(list(range(10**5)))[:20])"
run_both "python workload" python3 -c '
import hashlib, random
random.seed(7)
d = {str(i) * (i % 50): [i] * (i % 300) for i in range(200000)}
s = sorted(d, key=len)[:5]
blob = b"".join(bytes(random.getrandbits(8) for _ in range(1000)) for _ in range(500))
print(len(d), s[:2], hashlib.sha256(blob).hexdigest()[:16])
for _ in range(20): big = bytearray(8 * 1024 * 1024); big[-1] = 1
print(sum(len(v) for v in d.values()))'

# 4. A compiler: cc1 does a lot of small allocations, and every child process inherits LD_PRELOAD.
cat >"$TMP/t.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
  char *s = malloc(64); strcpy(s, "compiled under arena");
  void *a; if (posix_memalign(&a, 4096, 100)) return 1;
  printf("%s %d\n", s, ((unsigned long)a % 4096) == 0);
  free(a); free(s); return 0;
}
EOF
run_both "gcc compile + run" bash -c "gcc -O2 -o '$TMP/t' '$TMP/t.c' && '$TMP/t'"

echo "preload_smoke: $fails failure(s)"
exit $((fails != 0))
