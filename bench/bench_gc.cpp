// T7 measurement: R2.5 -- "running concurrently with reads and writes without
// blocking foreground operations".
//
// "Without blocking" is a latency claim, and a mean would hide the thing it is
// about: a collector that stalls the foreground does it occasionally and
// briefly, which barely moves an average and moves p99.9 a lot.
//
// EVERY CONFIGURATION GETS A FRESH, IDENTICALLY-BUILT STORE.
//   The first version of this benchmark ran the configurations one after another
//   against the same store, so each one inherited a store the previous had
//   already cleaned up -- and duly reported that GC makes ingest FASTER. Same
//   class of mistake as the RSS measurement in T4 (CHALLENGES B9): a benchmark
//   whose cases are not independent measures its own ordering.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "dedupe/gc.h"
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

double pct(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))];
}

struct Latencies {
  std::vector<double> ingest_ms;
  std::vector<double> restore_ms;
  uint64_t cycles = 0;
};

// n must be large enough for the tail statistic to mean something: at n=400 the
// "p99.9" is just the second-worst sample and swings 0.76x-1.29x between runs.
// At n=2000, p99 has 20 samples above it and is stable. p99.9 still would not
// be, so it is not reported -- quoting a statistic the sample size cannot
// support is how benchmarks lie without anyone intending to.
constexpr int kOps = 2000;
constexpr int kSeedFiles = 120;

// Build an identical store, then drive the foreground with GC configured as
// given (nullptr = collector never runs).
Latencies measure(const std::string& base, const char* tag, const GcOptions* opts) {
  Latencies out;
  const std::string sp = base + "-" + tag;
  if (std::system(("rm -rf " + sp).c_str()) != 0) return out;

  auto opened = Store::open(sp);
  if (!opened.ok()) return out;
  Store& store = *opened.value();

  // Identical seed => identical store contents for every configuration.
  std::mt19937_64 rng(9876);
  std::vector<std::string> readable;
  {
    auto session = store.begin_ingest();
    if (!session.ok()) return out;
    for (int i = 0; i < kSeedFiles; i++) {
      const std::string name = "seed" + std::to_string(i);
      auto data = random_bytes(rng, 256 * 1024);
      if (!session.value().ingest_bytes(name, data).ok()) return out;
      if (i % 3 == 0) readable.push_back(name);
    }
    if (!session.value().finish().ok()) return out;
  }
  // Delete two thirds, so the collector has real work: entries to sweep and
  // containers to compact, not an idle scan.
  for (int i = 0; i < kSeedFiles; i++) {
    if (i % 3 != 0) (void)store.remove("seed" + std::to_string(i));
  }

  std::unique_ptr<Gc> gc;
  if (opts != nullptr) {
    gc = std::make_unique<Gc>(&store, *opts);
    gc->start();
  }

  std::mt19937_64 op_rng(4242);  // identical operation sequence too
  auto session = store.begin_ingest();
  if (!session.ok()) return out;
  for (int i = 0; i < kOps; i++) {
    const auto data = random_bytes(op_rng, 64 * 1024);
    auto t0 = Clock::now();
    (void)session.value().ingest_bytes(std::string("fg/") + std::to_string(i), data).ok();
    out.ingest_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

    const std::string& name = readable[op_rng() % readable.size()];
    t0 = Clock::now();
    (void)store.restore_bytes(name).ok();
    out.restore_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
  }
  (void)session.value().finish();
  if (gc) {
    gc->stop();
    out.cycles = gc->cycles();
  }
  opened.value().reset();
  if (std::system(("rm -rf " + sp).c_str()) != 0) return out;
  return out;
}

void report(const char* label, const Latencies& l, const Latencies* base) {
  std::printf("  %-22s ingest  p50 %6.2f  p90 %6.2f  p99 %6.2f  max %7.2f", label,
              pct(l.ingest_ms, 0.50), pct(l.ingest_ms, 0.90), pct(l.ingest_ms, 0.99),
              pct(l.ingest_ms, 1.0));
  if (base != nullptr) {
    std::printf("   [%.2fx / %.2fx / %.2fx]",
                pct(l.ingest_ms, 0.50) / pct(base->ingest_ms, 0.50),
                pct(l.ingest_ms, 0.90) / pct(base->ingest_ms, 0.90),
                pct(l.ingest_ms, 0.99) / pct(base->ingest_ms, 0.99));
  }
  std::printf("\n  %-22s restore p50 %6.2f  p90 %6.2f  p99 %6.2f  max %7.2f", "",
              pct(l.restore_ms, 0.50), pct(l.restore_ms, 0.90), pct(l.restore_ms, 0.99),
              pct(l.restore_ms, 1.0));
  if (base != nullptr) {
    std::printf("   [%.2fx / %.2fx / %.2fx]",
                pct(l.restore_ms, 0.50) / pct(base->restore_ms, 0.50),
                pct(l.restore_ms, 0.90) / pct(base->restore_ms, 0.90),
                pct(l.restore_ms, 0.99) / pct(base->restore_ms, 0.99));
  }
  if (l.cycles > 0) std::printf("   (%llu gc cycles)", static_cast<unsigned long long>(l.cycles));
  std::printf("\n\n");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string base = (argc > 1) ? argv[1] : "/tmp/dedupe-bench-gc";
  std::printf("dedupe T7 GC latency  (%d foreground ops, milliseconds; "
              "bracket = p50/p90/p99 vs GC idle)\n\n", kOps);

  const Latencies idle = measure(base, "idle", nullptr);
  report("gc idle", idle, nullptr);

  // Sweep-only isolates LOCK and CPU contention: mark + sweep touch every index
  // shard and do no container I/O at all.
  GcOptions sweep_only;
  sweep_only.interval = std::chrono::milliseconds(1);
  sweep_only.enable_compaction = false;
  report("gc sweep-only, 1ms", measure(base, "sweep", &sweep_only), &idle);

  // Full adds copy-forward compaction: reads, writes and fsyncs against the same
  // device the foreground is using. 1 ms means back-to-back cycles -- a
  // deliberately unrealistic worst case.
  GcOptions aggressive;
  aggressive.interval = std::chrono::milliseconds(1);
  report("gc full, 1ms (worst)", measure(base, "aggressive", &aggressive), &idle);

  // And the shipped default.
  GcOptions realistic;
  realistic.interval = std::chrono::milliseconds(2000);
  report("gc full, 2s (default)", measure(base, "default", &realistic), &idle);
  return 0;
}
