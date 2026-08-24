// Concurrent ingest and restore (SPEC S6, and the foundation T7's GC must not
// block).
//
// Built and run under TSan by scripts/check.sh. TSan is the point of this file:
// a data race in an index shard or a container writer would not necessarily
// produce a wrong answer on any given run, and a concurrency suite that only
// checks results is a suite that passes until production.
//
// The design being exercised (SPEC 3.5):
//   - each ingest thread owns its OWN active container, so appends never contend
//   - the index is 256 independently locked shards
//   - container readers are shared_ptr, so a reader in flight keeps its file alive
#include <atomic>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

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

// Content built from a shared pool, so different threads genuinely collide on
// the same chunks -- which is the interesting case, not the easy one.
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

constexpr int kThreads = 4;

}  // namespace

// Distinct files, one thread each. The baseline: does parallel ingest produce a
// store that verifies and restores?
TEST(concurrent_ingest_of_distinct_files) {
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::mt19937_64 seed_rng(testing::seed());
  std::vector<std::vector<uint8_t>> pool;
  for (int i = 0; i < 8; i++) pool.push_back(random_bytes(seed_rng, 30000));

  std::map<std::string, std::vector<uint8_t>> want;
  std::mutex want_mu;
  std::atomic<int> failures{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(testing::seed() + static_cast<uint64_t>(t) * 7919);
      auto session = store.begin_ingest();
      if (!session.ok()) { failures++; return; }
      for (int i = 0; i < 12; i++) {
        const std::string name = "t" + std::to_string(t) + "/f" + std::to_string(i);
        auto data = from_pool(pool, rng, 100000 + rng() % 200000);
        if (!session.value().ingest_bytes(name, data).ok()) { failures++; return; }
        std::lock_guard<std::mutex> lk(want_mu);
        want[name] = std::move(data);
      }
      if (!session.value().finish().ok()) failures++;
    });
  }
  for (auto& th : threads) th.join();
  CHECK_EQ(failures.load(), 0);

  for (const auto& [name, data] : want) {
    TCTX(name);
    auto got = store.restore_bytes(name);
    REQUIRE(got.ok());
    CHECK(got.value() == data);
  }
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
  for (const std::string& p : rep.value().problems) std::printf("  PROBLEM: %s\n", p.c_str());

  auto st = store.stats();
  REQUIRE(st.ok());
  std::printf("  [info] %d threads x 12 files: %llu unique chunks, %llu refs, ratio %.2fx, "
              "%llu containers\n",
              kThreads, static_cast<unsigned long long>(st.value().unique_chunks),
              static_cast<unsigned long long>(st.value().chunk_references),
              st.value().dedup_ratio(),
              static_cast<unsigned long long>(st.value().containers));
  // Per-thread containers: at least one per ingesting thread.
  CHECK_GE(st.value().containers, static_cast<uint64_t>(kThreads));
}

// The race that matters: several threads ingest the SAME content at the same
// time. All of them miss the index lookup, all of them write the chunk, and
// exactly one insert wins. The losers must keep the winner's location -- if a
// loser repointed the index at its own copy, a recipe written moments earlier by
// the winner would be left referencing a location nothing points to.
TEST(concurrent_ingest_of_identical_content) {
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::mt19937_64 rng(testing::seed() ^ 0x99);
  const auto shared = random_bytes(rng, 2u << 20);

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t] {
      auto session = store.begin_ingest();
      if (!session.ok()) { failures++; return; }
      for (int i = 0; i < 3; i++) {
        const std::string name = "same-" + std::to_string(t) + "-" + std::to_string(i);
        if (!session.value().ingest_bytes(name, shared).ok()) failures++;
      }
      if (!session.value().finish().ok()) failures++;
    });
  }
  for (auto& th : threads) th.join();
  CHECK_EQ(failures.load(), 0);

  for (int t = 0; t < kThreads; t++) {
    for (int i = 0; i < 3; i++) {
      const std::string name = "same-" + std::to_string(t) + "-" + std::to_string(i);
      TCTX(name);
      auto got = store.restore_bytes(name);
      REQUIRE(got.ok());
      CHECK(got.value() == shared);
    }
  }
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());

  auto st = store.stats();
  REQUIRE(st.ok());
  // Racing threads may each have written a copy before anyone's insert landed,
  // so some duplication is EXPECTED and is exactly what GC reclaims. What must
  // not happen is unbounded duplication: 12 identical files must not cost 12x.
  const double waste = static_cast<double>(st.value().physical_bytes) /
                       static_cast<double>(shared.size());
  std::printf("  [info] %d threads x 3 copies of 2 MiB: physical = %.2fx one copy "
              "(1.0 ideal, %d.0 = no dedup at all)\n",
              kThreads, waste, kThreads * 3);
  CHECK_LT(waste, static_cast<double>(kThreads));
}

// Readers and writers at the same time. A restore in flight must not be
// disturbed by a container being appended to or rolled over underneath it --
// this is the property GC will lean on in T7.
TEST(concurrent_restore_while_ingesting) {
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::mt19937_64 rng(testing::seed() ^ 0xAA);
  std::map<std::string, std::vector<uint8_t>> stable;
  for (int i = 0; i < 6; i++) {
    const std::string name = "stable" + std::to_string(i);
    auto data = random_bytes(rng, 400000);
    REQUIRE(store.ingest_bytes(name, data).ok());
    stable[name] = std::move(data);
  }

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<uint64_t> restores{0};

  std::vector<std::thread> readers;
  for (int t = 0; t < 3; t++) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        for (const auto& [name, want] : stable) {
          auto got = store.restore_bytes(name);
          if (!got.ok() || got.value() != want) {
            failures++;
            return;
          }
          restores++;
        }
      }
    });
  }

  std::vector<std::thread> writers;
  for (int t = 0; t < 2; t++) {
    writers.emplace_back([&, t] {
      std::mt19937_64 wrng(testing::seed() + static_cast<uint64_t>(t) * 104729);
      auto session = store.begin_ingest();
      if (!session.ok()) { failures++; return; }
      for (int i = 0; i < 10; i++) {
        const std::string name = "new-" + std::to_string(t) + "-" + std::to_string(i);
        auto data = random_bytes(wrng, 300000);
        if (!session.value().ingest_bytes(name, data).ok()) failures++;
      }
      if (!session.value().finish().ok()) failures++;
    });
  }
  for (auto& th : writers) th.join();
  stop.store(true);
  for (auto& th : readers) th.join();

  CHECK_EQ(failures.load(), 0);
  CHECK_GT(restores.load(), uint64_t{0});
  std::printf("  [info] %llu restores completed while 2 threads ingested\n",
              static_cast<unsigned long long>(restores.load()));

  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
}

// list/stats walk the recipe directory while other threads add and remove
// recipes. A concurrent unlink between list_dir and read_file is an ordinary
// race, not an error, and must not fail the call.
TEST(concurrent_metadata_scans_during_mutation) {
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::mt19937_64 rng(testing::seed() ^ 0xBB);
  const auto blob = random_bytes(rng, 60000);
  for (int i = 0; i < 10; i++) {
    REQUIRE(store.ingest_bytes("churn" + std::to_string(i), blob).ok());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<uint64_t> scans{0};

  std::thread scanner([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      if (!store.list().ok()) failures++;
      if (!store.stats().ok()) failures++;
      scans++;
    }
  });

  std::thread churner([&] {
    auto session = store.begin_ingest();
    if (!session.ok()) { failures++; return; }
    for (int round = 0; round < 40; round++) {
      const std::string name = "churn" + std::to_string(round % 10);
      if (!session.value().ingest_bytes(name, blob).ok()) failures++;
      (void)store.remove(name);  // may or may not exist; both are fine
    }
    if (!session.value().finish().ok()) failures++;
  });

  churner.join();
  stop.store(true);
  scanner.join();
  CHECK_EQ(failures.load(), 0);
  CHECK_GT(scans.load(), uint64_t{0});
  std::printf("  [info] %llu concurrent list+stats scans during churn, 0 failures\n",
              static_cast<unsigned long long>(scans.load()));
}

// Two threads publishing the SAME name concurrently. rename() is atomic, so one
// whole file must win -- a reader must never see a mixture of the two. With a
// fixed "<path>.tmp" both writers would have shared one temp file and
// interleaved their bytes (CHALLENGES B11).
TEST(concurrent_publish_of_the_same_name_is_atomic) {
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::mt19937_64 rng(testing::seed() ^ 0xCC);
  std::vector<std::vector<uint8_t>> versions;
  for (int t = 0; t < kThreads; t++) versions.push_back(random_bytes(rng, 150000 + t * 1000));

  std::atomic<int> failures{0};
  for (int round = 0; round < 6; round++) {
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
      threads.emplace_back([&, t] {
        auto session = store.begin_ingest();
        if (!session.ok()) { failures++; return; }
        if (!session.value().ingest_bytes("contended", versions[t]).ok()) failures++;
        if (!session.value().finish().ok()) failures++;
      });
    }
    for (auto& th : threads) th.join();

    // Whatever landed must be one of the versions, in full.
    auto got = store.restore_bytes("contended");
    REQUIRE(got.ok());
    bool matched = false;
    for (const auto& v : versions) {
      if (got.value() == v) matched = true;
    }
    TCTX("round=" << round);
    CHECK(matched);
  }
  CHECK_EQ(failures.load(), 0);
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
}

// S4 ACROSS THREADS -- the bug this task existed to find.
//
// Session A appends chunks and puts them in the index. Session B ingests content
// made of those same chunks: every lookup is a HIT, so B writes nothing of its
// own -- yet B's recipe references A's container. If B publishes without making
// A's container durable, a crash leaves B's file pointing at chunks that never
// reached the device. Single-threaded T5 could not expose this, because within
// one session you always sync your own container.
//
// Two halves, because the fix has to be correct in both directions:
//   (1) when A's container is ALREADY durable, B must not fsync it again --
//       otherwise every publish pays for a redundant sync;
//   (2) when it is NOT durable, B must fsync it before publishing.
//
// Half (2) is made deterministic by injecting a failure into A's own sync, which
// leaves exactly the state the race would produce: chunks indexed, container not
// durable. (Also a realistic state in its own right -- a failed ingest.)
TEST(publish_syncs_containers_owned_by_other_sessions) {
  testing::FaultGuard guard;
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::mt19937_64 rng(testing::seed() ^ 0xDD);
  const auto data_ok = random_bytes(rng, 2u << 20);
  const auto data_stranded = random_bytes(rng, 2u << 20);

  auto a = store.begin_ingest();
  REQUIRE(a.ok());

  // --- (1) A publishes successfully, so its container is durable -------------
  auto ra = a.value().ingest_bytes("written-by-a", data_ok);
  REQUIRE(ra.ok());
  CHECK_GT(ra.value().chunks_new, uint64_t{0});
  {
    const uint64_t before = store.forced_syncs();
    auto b = store.begin_ingest();
    REQUIRE(b.ok());
    auto rb = b.value().ingest_bytes("deduped-by-b", data_ok);
    REQUIRE(rb.ok());
    CHECK_EQ(rb.value().chunks_new, uint64_t{0});
    CHECK(b.value().finish().ok());
    // Nothing to do: A already made those bytes durable when it published.
    CHECK_EQ(store.forced_syncs(), before);
  }

  // --- (2) A's own sync fails, leaving chunks indexed but not durable --------
  faults::reset();
  faults::fail_nth(faults::Op::kFsync, 1);
  auto failed = a.value().ingest_bytes("doomed", data_stranded);
  faults::reset();
  CHECK(!failed.ok());  // no recipe published

  // ...but the chunks it wrote ARE in the index, so B will dedup against them.
  const uint64_t before = store.forced_syncs();
  auto b = store.begin_ingest();
  REQUIRE(b.ok());
  auto rb = b.value().ingest_bytes("deduped-from-unsynced", data_stranded);
  REQUIRE(rb.ok());
  CHECK_EQ(rb.value().chunks_new, uint64_t{0});
  CHECK_EQ(rb.value().physical_bytes, uint64_t{0});
  const uint64_t after = store.forced_syncs();

  std::printf("  [info] B wrote 0 bytes but forced %llu sync(s) of A's container\n",
              static_cast<unsigned long long>(after - before));
  // The whole point: B had to make someone else's data durable before claiming
  // its own file exists.
  CHECK_GT(after, before);

  CHECK(b.value().finish().ok());
  CHECK(a.value().finish().ok());

  for (const char* n : {"written-by-a", "deduped-by-b", "deduped-from-unsynced"}) {
    TCTX(n);
    auto got = store.restore_bytes(n);
    REQUIRE(got.ok());
  }
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
}

RUN_ALL()
