// Background space reclamation (SPEC R2.1-R2.3, S2, S3).
//
// The invariant under test is S2: GC must never free a chunk that any surviving
// recipe references. Every test here ends by restoring everything that should
// still exist and byte-comparing it -- because the failure mode of a GC bug is
// not a crash, it is a file that quietly cannot be read any more.
#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "dedupe/gc.h"
#include "dedupe/gc_journal.h"
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

uint64_t physical(Store& store) {
  auto st = store.stats();
  return st.ok() ? st.value().physical_bytes : 0;
}

}  // namespace

TEST(gc_on_an_empty_store_is_a_no_op) {
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Gc gc(opened.value().get());
  auto r = gc.run_once();
  REQUIRE(r.ok());
  CHECK_EQ(r.value().swept, uint64_t{0});
  CHECK_EQ(r.value().containers_deleted, uint64_t{0});
}

// The most important negative result: with nothing deleted, GC must free
// NOTHING. A collector that is too eager looks like it works until you try to
// read something back.
TEST(gc_frees_nothing_when_nothing_was_deleted) {
  std::mt19937_64 rng(testing::seed());
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::map<std::string, std::vector<uint8_t>> want;
  for (int i = 0; i < 5; i++) {
    const std::string n = "f" + std::to_string(i);
    auto data = random_bytes(rng, 300000);
    REQUIRE(store.ingest_bytes(n, data).ok());
    want[n] = std::move(data);
  }
  const uint64_t before = physical(store);

  Gc gc(&store);
  auto r = gc.run_once();
  REQUIRE(r.ok());
  CHECK_EQ(r.value().swept, uint64_t{0});
  CHECK_EQ(r.value().containers_deleted, uint64_t{0});
  CHECK_EQ(r.value().containers_compacted, uint64_t{0});
  CHECK_EQ(physical(store), before);

  for (const auto& [n, data] : want) {
    TCTX(n);
    auto got = store.restore_bytes(n);
    REQUIRE(got.ok());
    CHECK(got.value() == data);
  }
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
}

// R2.2 -- deleting a file and running GC must actually shrink the store.
TEST(gc_reclaims_space_from_a_deleted_file) {
  std::mt19937_64 rng(testing::seed() ^ 0x11);
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  const auto keep_data = random_bytes(rng, 400000);
  const auto drop_data = random_bytes(rng, 3u << 20);  // unique: nothing shares it
  REQUIRE(store.ingest_bytes("keep", keep_data).ok());
  REQUIRE(store.ingest_bytes("drop", drop_data).ok());
  const uint64_t before = physical(store);

  CHECK(store.remove("drop").ok());
  // Deleting a recipe alone frees nothing -- that is the documented contract.
  CHECK_EQ(physical(store), before);

  Gc gc(&store);
  auto r = gc.run_once();
  REQUIRE(r.ok());
  const uint64_t after = physical(store);
  std::printf("  [info] physical %llu -> %llu (%lld reclaimed), swept %llu entries, "
              "%llu container(s) deleted, %llu compacted\n",
              static_cast<unsigned long long>(before), static_cast<unsigned long long>(after),
              static_cast<long long>(before - after),
              static_cast<unsigned long long>(r.value().swept),
              static_cast<unsigned long long>(r.value().containers_deleted),
              static_cast<unsigned long long>(r.value().containers_compacted));
  CHECK_LT(after, before);
  CHECK_GT(r.value().swept, uint64_t{0});

  // And the survivor is untouched.
  auto got = store.restore_bytes("keep");
  REQUIRE(got.ok());
  CHECK(got.value() == keep_data);
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
}

// S2, the core case: two files SHARE chunks. Deleting one must not free the
// chunks the other still needs. A refcount-free collector that got this wrong
// would silently destroy the survivor.
TEST(gc_never_frees_chunks_a_surviving_file_still_references) {
  std::mt19937_64 rng(testing::seed() ^ 0x22);
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  const auto shared = random_bytes(rng, 2u << 20);
  std::vector<uint8_t> a = shared;
  std::vector<uint8_t> b = shared;
  a.insert(a.end(), 50000, 0xAA);  // each has a little unique content too
  b.insert(b.end(), 50000, 0xBB);

  REQUIRE(store.ingest_bytes("a", a).ok());
  auto second = store.ingest_bytes("b", b);
  REQUIRE(second.ok());
  CHECK_GT(second.value().chunks_deduped, uint64_t{0});  // they really do share

  CHECK(store.remove("a").ok());
  Gc gc(&store);
  auto r = gc.run_once();
  REQUIRE(r.ok());

  // b must be byte-perfect. This is the assertion the whole design exists for.
  auto got = store.restore_bytes("b");
  REQUIRE(got.ok());
  CHECK(got.value() == b);
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());

  // Running it again must be stable, not progressively destructive.
  auto again = gc.run_once();
  REQUIRE(again.ok());
  CHECK_EQ(again.value().swept, uint64_t{0});
  auto got2 = store.restore_bytes("b");
  REQUIRE(got2.ok());
  CHECK(got2.value() == b);
}

// Copy-forward compaction: one container holding both files, one deleted, so the
// container is mostly dead but not entirely. Its live chunks must be copied into
// a fresh container and the index repointed.
TEST(gc_compacts_a_mostly_dead_container) {
  std::mt19937_64 rng(testing::seed() ^ 0x33);
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  const auto big = random_bytes(rng, 4u << 20);
  const auto small = random_bytes(rng, 512 * 1024);
  {
    // ONE session, so both files land in the same container.
    auto session = store.begin_ingest();
    REQUIRE(session.ok());
    REQUIRE(session.value().ingest_bytes("big", big).ok());
    REQUIRE(session.value().ingest_bytes("small", small).ok());
    CHECK(session.value().finish().ok());
  }
  auto ids = store.container_ids();
  REQUIRE(ids.ok());
  CHECK_EQ(ids.value().size(), size_t{1});

  CHECK(store.remove("big").ok());  // ~89% of the container is now dead
  const uint64_t before = physical(store);

  Gc gc(&store);
  auto r = gc.run_once();
  REQUIRE(r.ok());
  std::printf("  [info] compacted %llu container(s), copied %llu chunks, %llu -> %llu bytes\n",
              static_cast<unsigned long long>(r.value().containers_compacted),
              static_cast<unsigned long long>(r.value().chunks_copied),
              static_cast<unsigned long long>(before),
              static_cast<unsigned long long>(physical(store)));
  CHECK_EQ(r.value().containers_compacted, uint64_t{1});
  CHECK_GT(r.value().chunks_copied, uint64_t{0});
  CHECK_LT(physical(store), before / 2);

  // The survivor restores from its NEW location.
  auto got = store.restore_bytes("small");
  REQUIRE(got.ok());
  CHECK(got.value() == small);
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());

  // The old container is gone and a new one took its place.
  auto ids2 = store.container_ids();
  REQUIRE(ids2.ok());
  CHECK_EQ(ids2.value().size(), size_t{1});
  CHECK_NE(ids2.value()[0], ids.value()[0]);
}

// S3 -- a crash DURING compaction, before the commit record is durable. The
// destination is an incomplete copy and the source is untouched, so recovery
// must throw the copy away and change nothing else.
TEST(gc_crash_before_commit_discards_the_incomplete_copy) {
  testing::FaultGuard guard;
  std::mt19937_64 rng(testing::seed() ^ 0x44);
  testing::TempDir dir;
  const std::string sp = dir.file("store");
  const auto big = random_bytes(rng, 4u << 20);
  const auto small = random_bytes(rng, 512 * 1024);
  uint32_t original = 0;

  {
    auto opened = Store::open(sp);
    REQUIRE(opened.ok());
    Store& store = *opened.value();
    {
      auto session = store.begin_ingest();
      REQUIRE(session.ok());
      REQUIRE(session.value().ingest_bytes("big", big).ok());
      REQUIRE(session.value().ingest_bytes("small", small).ok());
      CHECK(session.value().finish().ok());
    }
    CHECK(store.remove("big").ok());

    auto ids_before = store.container_ids();
    REQUIRE(ids_before.ok());
    REQUIRE(ids_before.value().size() == 1);
    original = ids_before.value()[0];

    // fsync #1 is the BEGIN journal record; #2 is the destination container's
    // own sync. Failing from #2 leaves BEGIN durable and the destination
    // INCOMPLETE, with no COMMIT -- exactly the window this test is about.
    //
    // (An earlier version failed from #3, which is the COMMIT record's fsync.
    // That write had already reached the page cache, so recovery could still
    // read it and took the "finished" branch instead. Both branches are safe,
    // but the test was then not testing the one it named.)
    faults::reset();
    faults::fail_from(faults::Op::kFsync, 2);
    Gc gc(&store);
    auto r = gc.run_once();
    CHECK(!r.ok());
    faults::reset();
  }

  // Reopen: recovery replays the journal.
  auto reopened = Store::open(sp);
  REQUIRE(reopened.ok());
  auto got = reopened.value()->restore_bytes("small");
  REQUIRE(got.ok());
  CHECK(got.value() == small);
  auto rep = reopened.value()->verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
  for (const std::string& p : rep.value().problems) std::printf("  PROBLEM: %s\n", p.c_str());

  // Prove recovery took the UNDO branch: the original container survived and the
  // half-written copy is gone.
  auto ids_after = reopened.value()->container_ids();
  REQUIRE(ids_after.ok());
  CHECK_EQ(ids_after.value().size(), size_t{1});
  CHECK_EQ(ids_after.value()[0], original);

  // And GC still works afterwards -- recovery left a consistent store, not a
  // merely readable one.
  Gc gc2(reopened.value().get());
  auto r2 = gc2.run_once();
  REQUIRE(r2.ok());
  auto got2 = reopened.value()->restore_bytes("small");
  REQUIRE(got2.ok());
  CHECK(got2.value() == small);
}

// S3 -- a crash AFTER the commit record is durable but before the source is
// unlinked. The destination is authoritative; recovery must finish the job.
TEST(gc_crash_after_commit_finishes_the_compaction) {
  testing::FaultGuard guard;
  std::mt19937_64 rng(testing::seed() ^ 0x55);
  testing::TempDir dir;
  const std::string sp = dir.file("store");
  const auto big = random_bytes(rng, 4u << 20);
  const auto small = random_bytes(rng, 512 * 1024);
  uint32_t original = 0;

  {
    auto opened = Store::open(sp);
    REQUIRE(opened.ok());
    Store& store = *opened.value();
    {
      auto session = store.begin_ingest();
      REQUIRE(session.ok());
      REQUIRE(session.value().ingest_bytes("big", big).ok());
      REQUIRE(session.value().ingest_bytes("small", small).ok());
      CHECK(session.value().finish().ok());
    }
    CHECK(store.remove("big").ok());
    auto ids_before = store.container_ids();
    REQUIRE(ids_before.ok());
    REQUIRE(ids_before.value().size() == 1);
    original = ids_before.value()[0];

    // Fail the unlink of the source: COMMIT is already durable at that point.
    faults::reset();
    faults::fail_from(faults::Op::kUnlink, 1);
    Gc gc(&store);
    auto r = gc.run_once();
    CHECK(!r.ok());
    faults::reset();
  }

  auto reopened = Store::open(sp);
  REQUIRE(reopened.ok());
  auto got = reopened.value()->restore_bytes("small");
  REQUIRE(got.ok());
  CHECK(got.value() == small);
  auto rep = reopened.value()->verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
  for (const std::string& p : rep.value().problems) std::printf("  PROBLEM: %s\n", p.c_str());
  // Prove recovery took the FINISH branch: the source was unlinked, so what
  // survives is the compacted copy under a new id.
  auto ids = reopened.value()->container_ids();
  REQUIRE(ids.ok());
  CHECK_EQ(ids.value().size(), size_t{1});
  CHECK_NE(ids.value()[0], original);
}

// S7 -- GC walks the index in bounded batches. A tiny batch size must not change
// any outcome, only how often the lock is released.
TEST(gc_is_correct_with_a_tiny_scan_batch) {
  std::mt19937_64 rng(testing::seed() ^ 0x66);
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::map<std::string, std::vector<uint8_t>> want;
  for (int i = 0; i < 8; i++) {
    const std::string n = "f" + std::to_string(i);
    auto data = random_bytes(rng, 200000);
    REQUIRE(store.ingest_bytes(n, data).ok());
    want[n] = std::move(data);
  }
  for (int i = 0; i < 8; i += 2) {
    CHECK(store.remove("f" + std::to_string(i)).ok());
    want.erase("f" + std::to_string(i));
  }

  GcOptions opts;
  opts.scan_batch = 1;  // one slot per lock acquisition
  Gc gc(&store, opts);
  auto r = gc.run_once();
  REQUIRE(r.ok());
  CHECK_GT(r.value().swept, uint64_t{0});

  for (const auto& [n, data] : want) {
    TCTX(n);
    auto got = store.restore_bytes(n);
    REQUIRE(got.ok());
    CHECK(got.value() == data);
  }
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
}

// Re-ingesting content that GC just collected must work. The chunks are gone
// from both index and containers, so this is an ordinary write -- but a
// half-swept index (entry erased, chunk still referenced, or vice versa) would
// show up here as a corrupt restore.
TEST(gc_then_reingest_the_same_content) {
  std::mt19937_64 rng(testing::seed() ^ 0x77);
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  const auto data = random_bytes(rng, 1u << 20);
  REQUIRE(store.ingest_bytes("x", data).ok());
  CHECK(store.remove("x").ok());

  Gc gc(&store);
  REQUIRE(gc.run_once().ok());
  CHECK_EQ(store.index().size(), size_t{0});  // everything really was collected

  auto again = store.ingest_bytes("x", data);
  REQUIRE(again.ok());
  CHECK_GT(again.value().chunks_new, uint64_t{0});  // written afresh, not deduped
  auto got = store.restore_bytes("x");
  REQUIRE(got.ok());
  CHECK(got.value() == data);
  auto rep = store.verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
}

// R2.5 / S7 -- a GROSS-regression guard, not a measurement.
//
// bench_gc is where the real numbers live (p50/p90/p99 over 2000 ops against
// independently built stores). A precise latency assertion inside ctest would be
// flaky, so this bound is deliberately loose: it exists to catch a change that
// makes GC take a global lock or hold a shard lock across I/O, which would show
// up as an order of magnitude, not 20%.
TEST(gc_does_not_grossly_inflate_foreground_latency) {
  // Sanitizers do not slow every thread by the same factor, so a wall-clock
  // comparison between "GC idle" and "GC running" measures the sanitizer, not
  // the collector. It flaked exactly once under TSan before this guard went in.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
  std::printf("  [skip] wall-clock latency comparison is meaningless under a sanitizer\n");
  return;
#else
  std::mt19937_64 rng(testing::seed() ^ 0x99);
  auto measure = [&](bool with_gc) {
    testing::TempDir dir;
    auto opened = Store::open(dir.file("store"));
    if (!opened.ok()) return 0.0;
    Store& store = *opened.value();
    std::mt19937_64 seed_rng(4242);
    {
      auto session = store.begin_ingest();
      if (!session.ok()) return 0.0;
      for (int i = 0; i < 40; i++) {
        (void)session.value().ingest_bytes("seed" + std::to_string(i),
                                           random_bytes(seed_rng, 128 * 1024)).ok();
      }
      (void)session.value().finish();
    }
    for (int i = 0; i < 40; i++) {
      if (i % 3 != 0) (void)store.remove("seed" + std::to_string(i));
    }

    GcOptions opts;
    opts.interval = std::chrono::milliseconds(1);
    Gc gc(&store, opts);
    if (with_gc) gc.start();

    std::mt19937_64 op_rng(77);
    std::vector<double> ms;
    auto session = store.begin_ingest();
    if (!session.ok()) return 0.0;
    for (int i = 0; i < 150; i++) {
      const auto data = random_bytes(op_rng, 32 * 1024);
      const auto t0 = std::chrono::steady_clock::now();
      (void)session.value().ingest_bytes("fg" + std::to_string(i), data).ok();
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             t0).count());
    }
    (void)session.value().finish();
    if (with_gc) gc.stop();
    std::sort(ms.begin(), ms.end());
    return ms[static_cast<size_t>(0.90 * static_cast<double>(ms.size() - 1))];  // p90
  };

  const double quiet = measure(false);
  const double busy = measure(true);
  REQUIRE(quiet > 0.0);
  std::printf("  [info] foreground ingest p90: %.3f ms idle vs %.3f ms with GC running "
              "(%.2fx)\n", quiet, busy, busy / quiet);
  CHECK_LT(busy, quiet * 5.0);
#endif
}

RUN_ALL()
