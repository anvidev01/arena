#!/usr/bin/env bash
# Run a command inside the Arena Linux dev container, with the repo mounted at /arena.
#   scripts/dev.sh                      -> interactive shell
#   scripts/dev.sh scripts/check.sh     -> build + test inside the container
# The image is built on first use; rebuild with `docker build -t arena-dev docker/`.
set -euo pipefail

IMAGE=arena-dev
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -t "$IMAGE" "$ROOT/docker"
fi

TTY=()
if [ -t 0 ] && [ -t 1 ]; then TTY=(-it); fi
[ $# -eq 0 ] && set -- bash

# SYS_PTRACE: gdb and the sanitizers' stack unwinding.
# seccomp=unconfined: TSan re-executes itself with personality(ADDR_NO_RANDOMIZE).
exec docker run --rm ${TTY[@]+"${TTY[@]}"} \
  --cap-add=SYS_PTRACE --security-opt seccomp=unconfined \
  -v "$ROOT":/arena -w /arena "$IMAGE" "$@"
