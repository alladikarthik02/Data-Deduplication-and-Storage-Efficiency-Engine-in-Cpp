cmake_minimum_required(VERSION 3.22)
project(dedupe CXX)

set(CMAKE_CXX_STANDARD 20)          # std::span, <bit>, jthread, requires-expressions
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)       # -std=c++20, never -std=gnu++20: portability is a claim we make

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)  # -O2 -g: we benchmark optimized code, but keep symbols
endif()

# -Werror=unused-result is not style, it is SPEC S11: an ignored write()/fsync()
# return value is exactly how storage engines lose data silently.
add_compile_options(
  -Wall -Wextra -Wpedantic -Wshadow
  -Werror=unused-result
  -Werror=return-type
)

# Sanitizer configuration, selected by scripts/check.sh (SPEC S5, S6).
#   -DDEDUPE_SANITIZE=address  -> ASan + UBSan
#   -DDEDUPE_SANITIZE=thread   -> TSan
set(DEDUPE_SANITIZE "none" CACHE STRING "none | address | thread")
if(DEDUPE_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
elseif(DEDUPE_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
elseif(NOT DEDUPE_SANITIZE STREQUAL "none")
  message(FATAL_ERROR "DEDUPE_SANITIZE must be none|address|thread, got '${DEDUPE_SANITIZE}'")
endif()

include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests)

# --- core library -----------------------------------------------------------
# Everything with a real implementation lives here; the primitives (sha256,
# crc32c, chunker) stay header-only because they are hot and want inlining.
add_library(dedupe_core STATIC
  src/io.cpp
  src/format.cpp
  src/container.cpp
  src/index.cpp
)
target_link_libraries(dedupe_core PUBLIC pthread)

enable_testing()

# One line per suite. Keeps later tasks from touching build plumbing.
function(dedupe_test name)
  add_executable(${name} tests/${name}.cpp ${ARGN})
  target_link_libraries(${name} PRIVATE dedupe_core pthread)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# --- T0 ---------------------------------------------------------------------
dedupe_test(test_harness)

# --- T1: fingerprinting -----------------------------------------------------
dedupe_test(test_sha256)
dedupe_test(test_crc32c)

# --- T2: chunking ------------------------------------------------------------
dedupe_test(test_chunker)

# --- T3: on-disk formats and I/O ---------------------------------------------
dedupe_test(test_io)
dedupe_test(test_format)
dedupe_test(test_container)

# --- T4: the fingerprint index -----------------------------------------------
dedupe_test(test_index)

# Benchmarks are built, not run, by ctest -- they are measurements, not pass/fail
# gates. Run them explicitly:  ./build-none/bench_hash
add_executable(bench_hash bench/bench_hash.cpp)
target_link_libraries(bench_hash PRIVATE dedupe_core)
add_executable(bench_chunker bench/bench_chunker.cpp)
target_link_libraries(bench_chunker PRIVATE dedupe_core)
add_executable(bench_index bench/bench_index.cpp)
target_link_libraries(bench_index PRIVATE dedupe_core)
