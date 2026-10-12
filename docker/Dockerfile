# Arena development environment (D-001).
# Native AArch64 Linux on the M4 Mac; the same file builds an x86-64 image on an x86 host.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# libclang-rt-18-dev holds the ASan/TSan runtimes. It is only a *Recommends* of clang,
# so --no-install-recommends silently drops it unless listed explicitly (B-001).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang lld cmake ninja-build \
        libclang-rt-18-dev \
        gdb valgrind strace file time git python3 ca-certificates \
        libjemalloc-dev libmimalloc-dev \
        redis-server redis-tools \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang CXX=clang++
WORKDIR /arena
