// T2 measurement: how fast is the chunking path, and what does the streaming
// buffer copy cost?
//
// Relevant to SPEC R3.3. Chunking is unavoidable work on every ingested byte,
// exactly like fingerprinting, so it sits on the same critical path that
// SHA-256's hardware backend just unblocked (BENCHMARKS T1).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "dedupe/chunker.h"
#include "dedupe/sha256.h"

using namespace dedupe;
using Clock = std::chrono::steady_clock;

namespace {

template <class F>
double best_mb_per_s(size_t bytes, int reps, F&& f) {
  double best = 0;
  for (int r = 0; r < reps; r++) {
    const auto t0 = Clock::now();
    const uint64_t sink = f();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    asm volatile("" ::"r"(sink) : "memory");
    if (secs > 0) best = std::max(best, (static_cast<double>(bytes) / 1048576.0) / secs);
  }
  return best;
}

void row(const char* name, double mb_s) { std::printf("  %-34s %8.1f MB/s\n", name, mb_s); }

}  // namespace

int main() {
  std::mt19937_64 rng(12345);
  const size_t kSize = 64u << 20;
  std::vector<uint8_t> data(kSize);
  for (size_t i = 0; i + 8 <= kSize; i += 8) {
    const uint64_t r = rng();
    std::memcpy(data.data() + i, &r, 8);
  }
  const ChunkerParams p;

  std::printf("dedupe T2 chunking  (buffer = %zu MiB, best of 5)\n\n", kSize >> 20);

  // 1. The raw cut-point scan: no copying, no buffering. The algorithm's ceiling.
  size_t nchunks = 0;
  row("find_cut_point (zero-copy scan)", best_mb_per_s(kSize, 5, [&] {
        size_t pos = 0, n = 0;
        while (pos < data.size()) {
          pos += find_cut_point(ByteSpan(data.data() + pos, data.size() - pos), p);
          n++;
        }
        nchunks = n;
        return static_cast<uint64_t>(n);
      }));

  // 2. The streaming path as ingest will actually use it: 1 MiB reads fed into
  //    Chunker, which copies into its carry buffer. The delta against row 1 is
  //    the price of that copy.
  row("Chunker::feed (1 MiB reads)", best_mb_per_s(kSize, 5, [&] {
        Chunker ch(p);
        size_t n = 0;
        auto emit = [&](ByteSpan c) { n += c.size(); };
        for (size_t pos = 0; pos < data.size(); pos += (1u << 20)) {
          ch.feed(ByteSpan(data.data() + pos, std::min<size_t>(1u << 20, data.size() - pos)),
                  emit);
        }
        ch.finish(emit);
        return static_cast<uint64_t>(n);
      }));

  // 3. Chunk + fingerprint: the real ingest CPU cost per byte, minus I/O.
  row("chunk + sha256 (full ingest CPU)", best_mb_per_s(kSize, 5, [&] {
        Chunker ch(p);
        uint64_t acc = 0;
        auto emit = [&](ByteSpan c) { acc += sha256(c)[0]; };
        for (size_t pos = 0; pos < data.size(); pos += (1u << 20)) {
          ch.feed(ByteSpan(data.data() + pos, std::min<size_t>(1u << 20, data.size() - pos)),
                  emit);
        }
        ch.finish(emit);
        return acc;
      }));

  std::printf("\n  chunks: %zu   mean size: %.0f B (target %u B)\n", nchunks,
              static_cast<double>(kSize) / static_cast<double>(nchunks), p.avg_size);
  return 0;
}
