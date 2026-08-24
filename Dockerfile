# dedupe — reproducible build/test environment.
#
# WHY A CONTAINER AT ALL:
#   This engine's correctness claims are all about the filesystem: fsync ordering,
#   atomic rename, torn-tail recovery after kill -9, flock semantics, sparse-file
#   behaviour. Every one of those is OS- and filesystem-specific. macOS/APFS and
#   Linux/ext4 disagree on several of them. Pinning the OS is correctness, not tidiness.
#
# WHY UBUNTU 24.04:
#   glibc 2.39 + GCC 13 — full C++20 (std::span, jthread, <bit>) without a toolchain
#   scavenger hunt, and a mainstream LTS that a reviewer can reproduce exactly.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain plus the tools we will actually need when this breaks at 2am:
#   gdb            - inspecting a deadlock's thread stacks, a corrupted index entry
#   valgrind       - memcheck/helgrind as a second opinion to the sanitizers
#   binutils/file  - hexdump-adjacent forensics on our own container files
#   python3        - benchmark plotting / corpus inspection (not a build dependency)
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      g++ \
      cmake \
      ninja-build \
      gdb \
      valgrind \
      binutils \
      file \
      xxd \
      python3 \
      git \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# Root inside a throwaway, network-less dev container. Documented so it reads as a
# decision, not an oversight: we need to kill -9 our own children in crash tests and
# to drop caches for honest I/O benchmarks.
CMD ["/bin/bash"]
