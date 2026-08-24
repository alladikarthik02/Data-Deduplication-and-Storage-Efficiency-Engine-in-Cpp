// T6 measurement: does ingest actually scale with threads?
//
// The design claim (SPEC 3.5) is that giving each ingest thread its own active
// container removes the global append bottleneck, and that 256 index shards keep
// lock contention off the hot path. Either that shows up as near-linear scaling
// or the design does not do what it says.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dedupe/io.h"
#include "dedupe/store.h"

using namespace dedupe;
using Clock = std::chrono::steady_clock;

namespace {

std::vector<uint8_t> random_bytes(std::mt19937_64& rng, size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i + 8 <= n; i += 8) {
    const uint64_t r = rng();
    std::memcpy(v.data() + i, &r, 8);
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string base = (argc > 1) ? argv[1] : "/tmp/dedupe-bench-ingest";
  constexpr size_t kFileBytes = 8u << 20;
  constexpr int kFilesPerThread = 4;

  std::mt19937_64 rng(4242);
  // Unique data per file: this measures the WRITE path, not the dedup fast path.
  // Deduplicating input would make the benchmark look better and mean less.
  std::vector<std::vector<uint8_t>> payloads;
  for (int i = 0; i < 8 * kFilesPerThread; i++) payloads.push_back(random_bytes(rng, kFileBytes));

  std::printf("dedupe T6 ingest scaling  (%d x %zu MiB unique data per thread)\n\n",
              kFilesPerThread, kFileBytes >> 20);
  std::printf("  %8s %12s %12s %10s %12s %10s %10s\n", "threads", "MiB", "seconds", "MB/s",
              "speedup", "fsyncs", "writes");

  double single = 0;
  for (int threads : {1, 2, 4, 8}) {
    const std::string sp = base + "-" + std::to_string(threads);
    std::string rm = "rm -rf " + sp;
    if (std::system(rm.c_str()) != 0) return 1;

    auto opened = Store::open(sp);
    if (!opened.ok()) {
      std::fprintf(stderr, "open failed: %s\n", opened.status().to_string().c_str());
      return 1;
    }
    Store& store = *opened.value();

    std::atomic<uint64_t> total_bytes{0};
    std::atomic<int> failures{0};
    faults::reset();  // reuse the injector's counters as free instrumentation
    const auto t0 = Clock::now();
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++) {
      pool.emplace_back([&, t] {
        auto session = store.begin_ingest();
        if (!session.ok()) { failures++; return; }
        for (int i = 0; i < kFilesPerThread; i++) {
          const auto& data = payloads[static_cast<size_t>(t * kFilesPerThread + i) %
                                      payloads.size()];
          auto r = session.value().ingest_bytes(
              "t" + std::to_string(t) + "/f" + std::to_string(i), data);
          if (!r.ok()) { failures++; return; }
          total_bytes += r.value().logical_bytes;
        }
        if (!session.value().finish().ok()) failures++;
      });
    }
    for (auto& th : pool) th.join();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    if (failures.load() != 0) {
      std::fprintf(stderr, "%d ingest failures\n", failures.load());
      return 1;
    }
    const double mib = static_cast<double>(total_bytes.load()) / 1048576.0;
    const double mb_s = mib / secs;
    if (threads == 1) single = mb_s;
    std::printf("  %8d %12.1f %12.3f %10.1f %11.2fx %10llu %10llu\n", threads, mib, secs,
                mb_s, mb_s / single,
                static_cast<unsigned long long>(faults::count(faults::Op::kFsync)),
                static_cast<unsigned long long>(faults::count(faults::Op::kWrite)));
    std::string cleanup = "rm -rf " + sp;
    if (std::system(cleanup.c_str()) != 0) return 1;
  }
  return 0;
}
