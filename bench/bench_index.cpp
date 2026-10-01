// T4 measurement: the two claims in headline claim R3.
//
//   R3.1 "memory-efficient fingerprint index"  -> measured against the two
//        implementations a reasonable engineer would otherwise have written,
//        using real resident memory (RSS), not a theoretical byte count. RSS
//        includes allocator overhead and fragmentation, which is exactly what a
//        theoretical count would flatter away.
//
//   R3.2 "held deduplication throughput steady as the dataset grew" -> measured
//        at 1x, 2x, 4x, 8x, 16x scale. The failure this is looking for is a
//        rehash cliff or a probe-length blowup, either of which shows up as
//        throughput falling off as the store fills.
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "dedupe/index.h"
#include "dedupe/sha256.h"

using namespace dedupe;
using Clock = std::chrono::steady_clock;

namespace {

// MEASURING MEMORY HONESTLY -- harder than it looks.
//
// First attempt measured RSS before and after building each structure, all in
// one process. It reported that a 1M-entry std::unordered_map cost 11 MiB, which
// is arithmetically impossible (its nodes alone are ~64 B each). The reason:
// freeing the previous structure does NOT return its pages to the OS -- glibc
// keeps them on its own free lists -- so every case after the first allocated
// into already-resident memory and measured a delta of nearly zero. RSS deltas
// are only meaningful for the FIRST allocation in a process.
//
// Fix: run every case in its own forked child. Each child inherits a small
// baseline, builds exactly one structure, prints its row, and exits. See
// CHALLENGES B9.

// Resident set size in bytes, straight from the kernel. The honest measure:
// whatever the allocator really took from the OS, including its own headers.
size_t rss_bytes() {
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) return 0;
  long total = 0, resident = 0;
  const int n = std::fscanf(f, "%ld %ld", &total, &resident);
  std::fclose(f);
  if (n != 2) return 0;
  return static_cast<size_t>(resident) * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
}

struct DigestHash {
  size_t operator()(const Digest32& d) const {
    size_t h;
    std::memcpy(&h, d.data(), sizeof(h));
    return h;
  }
};

std::vector<Digest32> make_fingerprints(size_t n) {
  std::vector<Digest32> v;
  v.reserve(n);
  for (size_t i = 0; i < n; i++) {
    v.push_back(sha256(ByteSpan(reinterpret_cast<const uint8_t*>(&i), sizeof(i))));
  }
  return v;
}

Location loc_n(size_t i) {
  Location l;
  l.container_id = static_cast<uint32_t>(i % 1000);
  l.offset = static_cast<uint32_t>((i * 9294) % Location::kMaxOffset);
  l.length = 9294;
  return l;
}

double ns_per_op(double secs, size_t ops) { return secs * 1e9 / static_cast<double>(ops); }

// Run one measurement in a forked child so its RSS delta is real.
template <class F>
void in_child(F&& f) {
  std::fflush(stdout);  // or the parent's buffered output is duplicated into every child
  const pid_t pid = ::fork();
  if (pid == 0) {
    f();
    std::fflush(stdout);
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
}

}  // namespace

int main() {
  constexpr size_t kN = 1000000;  // 1M chunks ~= 8.9 GiB of logical data at 9294 B/chunk
  const auto fps = make_fingerprints(kN);

  std::printf("dedupe T4 index  (%zu fingerprints)\n\n", kN);
  std::printf("  %-34s %10s %10s %10s %10s\n", "implementation", "RSS", "B/entry", "insert",
              "lookup");
  std::printf("  %-34s %10s %10s %10s %10s\n", "", "(MiB)", "", "(ns/op)", "(ns/op)");

  auto report = [&](const char* name, size_t rss, double ins_s, double look_s) {
    std::printf("  %-34s %10.1f %10.1f %10.1f %10.1f\n", name,
                static_cast<double>(rss) / 1048576.0,
                static_cast<double>(rss) / static_cast<double>(kN), ns_per_op(ins_s, kN),
                ns_per_op(look_s, kN));
  };

  // --- ours ----------------------------------------------------------------
  in_child([&] {
    const size_t base = rss_bytes();
    Index idx(64);
    auto t0 = Clock::now();
    for (size_t i = 0; i < kN; i++) {
      auto r = idx.insert(fps[i], loc_n(i));
      if (!r.ok()) ::_exit(1);
    }
    const double ins = std::chrono::duration<double>(Clock::now() - t0).count();
    const size_t rss = rss_bytes() - base;

    t0 = Clock::now();
    uint64_t sink = 0;
    for (size_t i = 0; i < kN; i++) {
      Location out;
      if (idx.lookup(fps[i], &out)) sink += out.offset;
    }
    const double look = std::chrono::duration<double>(Clock::now() - t0).count();
    asm volatile("" ::"r"(sink) : "memory");
    report("dedupe::Index (sharded, flat)", rss, ins, look);
    const IndexStats st = idx.stats();
    std::printf("      %zu slots, load %.3f, max probe %zu\n", st.capacity, st.load_factor,
                st.max_probe);
  });

  // --- baseline 1: the obvious version --------------------------------------
  in_child([&] {
    const size_t base = rss_bytes();
    auto* m = new std::unordered_map<std::string, Location>();
    auto t0 = Clock::now();
    for (size_t i = 0; i < kN; i++) {
      m->emplace(std::string(reinterpret_cast<const char*>(fps[i].data()), 32), loc_n(i));
    }
    const double ins = std::chrono::duration<double>(Clock::now() - t0).count();
    const size_t rss = rss_bytes() - base;

    t0 = Clock::now();
    uint64_t sink = 0;
    for (size_t i = 0; i < kN; i++) {
      auto it = m->find(std::string(reinterpret_cast<const char*>(fps[i].data()), 32));
      if (it != m->end()) sink += it->second.offset;
    }
    const double look = std::chrono::duration<double>(Clock::now() - t0).count();
    asm volatile("" ::"r"(sink) : "memory");
    report("unordered_map<string,Location>", rss, ins, look);
    delete m;
  });

  // --- baseline 2: the fair version ------------------------------------------
  // No per-key heap allocation (the digest lives inline in the node), so this is
  // the strongest reasonable baseline rather than a straw man.
  in_child([&] {
    const size_t base = rss_bytes();
    auto* m = new std::unordered_map<Digest32, Location, DigestHash>();
    auto t0 = Clock::now();
    for (size_t i = 0; i < kN; i++) m->emplace(fps[i], loc_n(i));
    const double ins = std::chrono::duration<double>(Clock::now() - t0).count();
    const size_t rss = rss_bytes() - base;

    t0 = Clock::now();
    uint64_t sink = 0;
    for (size_t i = 0; i < kN; i++) {
      auto it = m->find(fps[i]);
      if (it != m->end()) sink += it->second.offset;
    }
    const double look = std::chrono::duration<double>(Clock::now() - t0).count();
    asm volatile("" ::"r"(sink) : "memory");
    report("unordered_map<Digest32,Location>", rss, ins, look);
    delete m;
  });

  // --- R3.2: does DEDUPLICATION throughput hold as the dataset grows? --------
  //
  // The bullet claims steady *deduplication* throughput, not steady index
  // latency, and those are different claims. Index lookups do slow down as the
  // table outgrows cache -- that is the memory hierarchy, not the algorithm, and
  // pretending otherwise would be dishonest. What matters is whether it moves
  // the number a user sees, so the last column measures the real thing: chunk a
  // buffer, fingerprint it, probe the index, at each store size.
  std::printf("\n  scaling -- as the store grows:\n");
  std::printf("  %12s %14s %14s %10s %8s %10s %12s\n", "entries", "insert (ns)",
              "lookup (ns)", "MiB", "B/entry", "max probe", "dedup MB/s");
  for (size_t n = kN / 16; n <= kN; n *= 2) {
   in_child([&] {
    const size_t base = rss_bytes();
    Index idx(64);
    auto t0 = Clock::now();
    for (size_t i = 0; i < n; i++) {
      auto r = idx.insert(fps[i], loc_n(i));
      if (!r.ok()) ::_exit(1);
    }
    const double ins = std::chrono::duration<double>(Clock::now() - t0).count();
    const size_t rss = rss_bytes() - base;

    t0 = Clock::now();
    uint64_t sink = 0;
    // Probe in a shuffled order so the measurement is real random access, not a
    // sequential walk the prefetcher would hide.
    std::mt19937_64 rng(7);
    for (size_t i = 0; i < n; i++) {
      Location out;
      if (idx.lookup(fps[rng() % n], &out)) sink += out.offset;
    }
    const double look = std::chrono::duration<double>(Clock::now() - t0).count();
    asm volatile("" ::"r"(sink) : "memory");
    const IndexStats st = idx.stats();

    // End-to-end dedup work: hash a mean-sized chunk, then probe. This is what
    // one chunk of ingest actually costs once the bytes are in hand.
    std::vector<uint8_t> chunk(9294);
    std::mt19937_64 crng(11);
    for (auto& b : chunk) b = static_cast<uint8_t>(crng() & 0xFF);
    constexpr size_t kOps = 20000;
    t0 = Clock::now();
    uint64_t sink2 = 0;
    for (size_t i = 0; i < kOps; i++) {
      chunk[i % chunk.size()] = static_cast<uint8_t>(i);  // defeat any caching
      const Digest32 d = sha256(chunk);
      Location out;
      if (idx.lookup(d, &out)) sink2 += out.offset;
      sink2 += d[0];
    }
    const double e2e = std::chrono::duration<double>(Clock::now() - t0).count();
    asm volatile("" ::"r"(sink2) : "memory");
    const double mb_s = (static_cast<double>(kOps) * 9294.0 / 1048576.0) / e2e;

    std::printf("  %12zu %14.1f %14.1f %10.1f %8.1f %10zu %12.1f\n", n, ns_per_op(ins, n),
                ns_per_op(look, n), static_cast<double>(rss) / 1048576.0,
                static_cast<double>(rss) / static_cast<double>(n), st.max_probe, mb_s);
   });
  }
  return 0;
}
