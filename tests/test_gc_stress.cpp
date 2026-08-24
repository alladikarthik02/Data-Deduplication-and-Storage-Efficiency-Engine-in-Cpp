// S2 under concurrent load -- the test résumé bullet #2 lives or dies on.
//
// "Background space reclamation ... running concurrently with reads and writes
// without blocking foreground operations" is only a claim until GC is actually
// running while files are being ingested, restored and deleted. This suite does
// that, then restores EVERY surviving file and byte-compares it.
//
// Built under TSan by scripts/check.sh as well, so a race between the collector
// and the foreground is caught even in the runs where it does not corrupt
// anything.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dedupe/gc.h"
#include "dedupe/store.h"
#include "test.h"
#include "tmpdir.h"

using namespace dedupe;

namespace {

std::vector<uint8_t> random_bytes(std::mt19937_64& rng, size_t n) {
  std::vector<uint8_t> v(n);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const uint64_t r = rng();
    std::memcpy(v.data() + i, &r, 8);
  }
  for (; i < n; i++) v[i] = static_cast<uint8_t>(rng() & 0xFF);
  return v;
}

// Content drawn from a shared pool so files genuinely share chunks -- which is
// what makes "did GC free something someone else still needs" a real question.
std::vector<uint8_t> from_pool(const std::vector<std::vector<uint8_t>>& pool,
                               std::mt19937_64& rng, size_t n) {
  std::vector<uint8_t> out;
  while (out.size() < n) {
    const auto& b = pool[rng() % pool.size()];
    out.insert(out.end(), b.begin(), b.end());
  }
  out.resize(n);
  return out;
}

}  // namespace

// THE test. Ingest, restore and delete threads all running while the background
// collector sweeps and compacts underneath them.
TEST(gc_stress_concurrent_ingest_restore_delete) {
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::mt19937_64 seed_rng(testing::seed());
  std::vector<std::vector<uint8_t>> pool;
  for (int i = 0; i < 12; i++) pool.push_back(random_bytes(seed_rng, 25000));

  // `shadow` is the oracle: what the store SHOULD contain. Guarded so the
  // checker thread never races the writers on the map itself.
  std::map<std::string, std::vector<uint8_t>> shadow;
  std::mutex shadow_mu;
  std::atomic<int> failures{0};
  std::atomic<uint64_t> restores{0}, ingests{0}, deletes{0};
  std::atomic<bool> stop{false};

  GcOptions opts;
  opts.interval = std::chrono::milliseconds(15);  // aggressive: maximize overlap
  opts.scan_batch = 64;
  Gc gc(&store, opts);
  gc.start();

  std::vector<std::thread> writers;
  for (int t = 0; t < 3; t++) {
    writers.emplace_back([&, t] {
      std::mt19937_64 rng(testing::seed() + static_cast<uint64_t>(t) * 7919);
      auto session = store.begin_ingest();
      if (!session.ok()) { failures++; return; }
      // Every ingest uses a NAME OF ITS OWN, deliberately.
      //
      // An earlier version reused eight names per thread, and the oracle became
      // unfalsifiable: between `ingest_bytes` returning and `shadow[name] = data`
      // there is a window where the store holds the NEW content while the oracle
      // still claims the old, so a reader legitimately sees a mismatch. Nine such
      // "failures" were reported against a perfectly correct engine
      // (CHALLENGES B14). Concurrent overwrite of one name is already covered by
      // T6's concurrent_publish_of_the_same_name_is_atomic; here it only
      // destroys the oracle's precision.
      std::vector<std::string> mine;
      for (int i = 0; i < 25; i++) {
        const std::string name = "t" + std::to_string(t) + "/f" + std::to_string(i);
        auto data = from_pool(pool, rng, 60000 + rng() % 200000);
        if (!session.value().ingest_bytes(name, data).ok()) { failures++; return; }
        {
          std::lock_guard<std::mutex> lk(shadow_mu);
          shadow[name] = data;
        }
        mine.push_back(name);
        ingests++;
        // Delete one of our own earlier files sometimes, so there is real garbage
        // for GC to find while everything else is still in flight.
        if (rng() % 3 == 0 && mine.size() > 2) {
          const std::string victim = mine[rng() % (mine.size() - 1)];
          std::lock_guard<std::mutex> lk(shadow_mu);
          auto it = shadow.find(victim);
          if (it != shadow.end() && store.remove(victim).ok()) {
            shadow.erase(it);
            deletes++;
          }
        }
      }
      if (!session.value().finish().ok()) failures++;
    });
  }

  // Readers continuously restore whatever the oracle says should exist. If GC
  // ever frees a live chunk, this is where it surfaces.
  std::vector<std::thread> readers;
  for (int t = 0; t < 2; t++) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        std::vector<std::pair<std::string, std::vector<uint8_t>>> snapshot;
        {
          std::lock_guard<std::mutex> lk(shadow_mu);
          for (const auto& kv : shadow) snapshot.push_back(kv);
        }
        for (const auto& [name, want] : snapshot) {
          auto got = store.restore_bytes(name);
          if (!got.ok()) {
            // The only legitimate failure is that the file was deleted between
            // taking the snapshot and reading it.
            std::lock_guard<std::mutex> lk(shadow_mu);
            if (shadow.count(name)) {
              std::fprintf(stderr, "  restore of live file '%s' failed: %s\n", name.c_str(),
                           got.status().to_string().c_str());
              failures++;
            }
            continue;
          }
          if (got.value() != want) {
            // Names are never reused, so a live file's bytes cannot legitimately
            // change. Any mismatch here is a real defect.
            std::fprintf(stderr, "  restore of '%s' returned wrong bytes (%zu vs %zu)\n",
                         name.c_str(), got.value().size(), want.size());
            failures++;
          }
          restores++;
        }
      }
    });
  }

  for (auto& th : writers) th.join();
  stop.store(true);
  for (auto& th : readers) th.join();
  gc.stop();

  std::printf("  [info] %llu ingests, %llu deletes, %llu restores, %llu gc cycles, 0 expected "
              "failures\n",
              static_cast<unsigned long long>(ingests.load()),
              static_cast<unsigned long long>(deletes.load()),
              static_cast<unsigned long long>(restores.load()),
              static_cast<unsigned long long>(gc.cycles()));
  CHECK_EQ(failures.load(), 0);
  CHECK_GT(gc.cycles(), uint64_t{0});
  CHECK_GT(restores.load(), uint64_t{0});

  // Final settled check: everything the oracle still lists must restore exactly.
  for (const auto& [name, want] : shadow) {
    TCTX(name);
    auto got = store.restore_bytes(name);
    REQUIRE(got.ok());
    CHECK(got.value() == want);
  }
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  for (const std::string& p : rep.value().problems) std::printf("  PROBLEM: %s\n", p.c_str());
  CHECK(rep.value().ok());
}

// The resurrection race, called out explicitly in SPEC 3.6: delete a file so its
// chunks become garbage, start a GC cycle, and re-ingest the SAME content while
// the collector is deciding those chunks are dead.
//
// The epoch fence is what makes this safe: re-ingest either writes the chunk
// afresh (new entry, current epoch) or dedup-hits the doomed entry -- and a hit
// stamps touch_epoch, which is exactly why lookup() stamps at all.
TEST(gc_stress_delete_then_reingest_the_same_content) {
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::mt19937_64 rng(testing::seed() ^ 0xBEEF);
  std::vector<std::vector<uint8_t>> payloads;
  for (int i = 0; i < 6; i++) payloads.push_back(random_bytes(rng, 400000));

  GcOptions opts;
  opts.interval = std::chrono::milliseconds(5);
  Gc gc(&store, opts);
  gc.start();

  std::atomic<int> failures{0};
  for (int round = 0; round < 30; round++) {
    const auto& data = payloads[static_cast<size_t>(round) % payloads.size()];
    const std::string name = "phoenix" + std::to_string(round % 3);
    if (!store.ingest_bytes(name, data).ok()) { failures++; break; }
    auto got = store.restore_bytes(name);
    if (!got.ok() || got.value() != data) { failures++; break; }
    if (!store.remove(name).ok()) { failures++; break; }
    // Immediately bring it back, while GC is very likely mid-cycle on it.
    if (!store.ingest_bytes(name, data).ok()) { failures++; break; }
    auto again = store.restore_bytes(name);
    if (!again.ok() || again.value() != data) {
      std::fprintf(stderr, "  round %d: resurrected file did not restore\n", round);
      failures++;
      break;
    }
  }
  gc.stop();
  CHECK_EQ(failures.load(), 0);
  std::printf("  [info] 30 delete/re-ingest rounds against a live collector, %llu gc cycles\n",
              static_cast<unsigned long long>(gc.cycles()));

  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  for (const std::string& p : rep.value().problems) std::printf("  PROBLEM: %s\n", p.c_str());
  CHECK(rep.value().ok());
}

RUN_ALL()
