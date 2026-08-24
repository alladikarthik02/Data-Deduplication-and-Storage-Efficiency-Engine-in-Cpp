// The fingerprint index (SPEC R3.1, R3.2, S8, S10).
//
// The test that matters most here is index_erase_keeps_every_other_key_findable.
// Backward-shift deletion is easy to get subtly wrong, and the failure mode is
// not a crash: keys whose probe chain ran through the erased slot silently become
// unreachable. In a dedup engine that means the index reports "new chunk" for
// data it already holds -- the store still restores correctly, it just quietly
// stops deduplicating. A bug that costs only disk space is exactly the kind that
// survives to production, so it gets the most punishing test in this file.
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <unordered_set>
#include <vector>

#include "dedupe/index.h"
#include "dedupe/sha256.h"
#include "test.h"

using namespace dedupe;

namespace {

Digest32 fp_n(uint64_t n) {
  return sha256(ByteSpan(reinterpret_cast<const uint8_t*>(&n), sizeof(n)));
}

Location loc_n(uint32_t n) {
  Location l;
  l.container_id = n % 1000;
  l.offset = (n * 4096) % Location::kMaxOffset;
  l.length = 1 + (n % 60000);
  return l;
}

}  // namespace

TEST(location_packing_round_trips_at_every_boundary) {
  const std::vector<Location> cases = {
      {0, 0, 1},
      {1, 4096, 8192},
      {Location::kMaxContainer, Location::kMaxOffset, Location::kMaxLength},
      {Location::kMaxContainer, 0, 1},
      {0, Location::kMaxOffset, Location::kMaxLength},
  };
  for (const auto& l : cases) {
    TCTX("c=" << l.container_id << " o=" << l.offset << " len=" << l.length);
    CHECK(l.fits());
    CHECK(Location::unpack(l.pack()) == l);
  }
  // Out-of-range values must be rejected at insert, not silently truncated --
  // a wrapped offset points at the wrong chunk and restores wrong data.
  for (const Location bad : {Location{Location::kMaxContainer + 1, 0, 1},
                             Location{0, Location::kMaxOffset + 1, 1},
                             Location{0, 0, Location::kMaxLength + 1}}) {
    CHECK(!bad.fits());
    Index idx;
    auto r = idx.insert(fp_n(1), bad);
    CHECK(!r.ok());
    CHECK(r.status().code() == Err::kInvalid);
  }
}

TEST(index_insert_lookup_basics) {
  Index idx;
  Location out;
  CHECK(!idx.lookup(fp_n(1), &out));
  CHECK_EQ(idx.size(), size_t{0});

  auto r = idx.insert(fp_n(1), loc_n(1));
  REQUIRE(r.ok());
  CHECK(r.value().inserted);
  CHECK_EQ(idx.size(), size_t{1});
  REQUIRE(idx.lookup(fp_n(1), &out));
  CHECK(out == loc_n(1));

  // Re-inserting an existing fingerprint reports the EXISTING location, not the
  // new one: the loser of an ingest race must not repoint the index away from
  // the copy that recipes already reference.
  auto again = idx.insert(fp_n(1), loc_n(999));
  REQUIRE(again.ok());
  CHECK(!again.value().inserted);
  CHECK(again.value().loc == loc_n(1));
  CHECK_EQ(idx.size(), size_t{1});
  REQUIRE(idx.lookup(fp_n(1), &out));
  CHECK(out == loc_n(1));
}

TEST(index_holds_many_keys_and_grows) {
  Index idx(8);  // start tiny so growth runs many times
  constexpr size_t kN = 200000;
  for (size_t i = 0; i < kN; i++) {
    auto r = idx.insert(fp_n(i), loc_n(static_cast<uint32_t>(i)));
    REQUIRE(r.ok());
    CHECK(r.value().inserted);
  }
  CHECK_EQ(idx.size(), kN);

  for (size_t i = 0; i < kN; i++) {
    Location out;
    if (!idx.lookup(fp_n(i), &out)) {
      TCTX("i=" << i);
      CHECK(false);
      break;
    }
    if (!(out == loc_n(static_cast<uint32_t>(i)))) {
      TCTX("i=" << i);
      CHECK(false);
      break;
    }
  }
  // Absent keys must stay absent -- a probe that wraps forever would hang here.
  for (size_t i = kN; i < kN + 1000; i++) CHECK(!idx.lookup(fp_n(i), nullptr));

  const IndexStats st = idx.stats();
  std::printf("  [info] %zu entries  cap=%zu  load=%.3f  max_probe=%zu  %.1f B/entry\n",
              st.entries, st.capacity, st.load_factor, st.max_probe,
              static_cast<double>(st.memory_bytes) / static_cast<double>(st.entries));
  CHECK_LE(st.load_factor, Index::kMaxLoad);
  // With 1.5x growth the load never falls below kMaxLoad/1.5 = 0.6, so bytes per
  // entry stays inside [35.6, 53.3]. Doubling instead of 1.5x allowed 0.45 and
  // 70+ B/entry (CHALLENGES B10), so this bound is what keeps R3.1 true.
  CHECK_GE(st.load_factor, 0.55);
  CHECK_LT(static_cast<double>(st.memory_bytes) / static_cast<double>(st.entries), 55.0);
  // Robin Hood bounds displacement. Plain linear probing measured 80 here; a
  // regression to that would mean the displacement logic broke.
  CHECK_LT(st.max_probe, size_t{40});
}

// THE deletion test. Erase a random half, then require that every remaining key
// is still findable. A naive "just blank the slot" implementation passes a
// simple insert/erase/lookup test and fails this one immediately.
TEST(index_erase_keeps_every_other_key_findable) {
  std::mt19937_64 rng(testing::seed());
  Index idx(8);
  constexpr size_t kN = 50000;
  for (size_t i = 0; i < kN; i++) {
    auto r = idx.insert(fp_n(i), loc_n(static_cast<uint32_t>(i)));
    REQUIRE(r.ok());
  }

  std::unordered_set<uint64_t> erased;
  for (size_t i = 0; i < kN; i++) {
    if (rng() % 2 == 0) {
      CHECK(idx.erase(fp_n(i)));
      erased.insert(i);
    }
  }
  CHECK_EQ(idx.size(), kN - erased.size());
  CHECK_EQ(idx.stats().entries, kN - erased.size());

  size_t missing = 0, resurrected = 0;
  for (size_t i = 0; i < kN; i++) {
    Location out;
    const bool found = idx.lookup(fp_n(i), &out);
    if (erased.count(i)) {
      if (found) resurrected++;
    } else {
      if (!found) missing++;
      else if (!(out == loc_n(static_cast<uint32_t>(i)))) missing++;
    }
  }
  CHECK_EQ(missing, size_t{0});
  CHECK_EQ(resurrected, size_t{0});

  // Erasing something absent is a no-op, not a corruption.
  CHECK(!idx.erase(fp_n(kN + 5)));

  // And the table is still usable: reinsert everything erased.
  for (uint64_t i : erased) {
    auto r = idx.insert(fp_n(i), loc_n(static_cast<uint32_t>(i) + 7));
    REQUIRE(r.ok());
    CHECK(r.value().inserted);
  }
  CHECK_EQ(idx.size(), kN);
}

// Interleaved random operations against a std::map oracle. This is the
// differential test for the whole structure: any divergence in any order of
// operations shows up as a mismatch.
TEST(index_matches_a_reference_map_under_random_operations) {
  std::mt19937_64 rng(testing::seed() ^ 0xAB);
  Index idx(8);
  std::map<uint64_t, Location> oracle;
  constexpr size_t kKeyspace = 5000;

  for (int op = 0; op < 200000; op++) {
    const uint64_t k = rng() % kKeyspace;
    switch (rng() % 4) {
      case 0: {  // insert
        const Location l = loc_n(static_cast<uint32_t>(rng() % 100000));
        auto r = idx.insert(fp_n(k), l);
        REQUIRE(r.ok());
        const auto it = oracle.find(k);
        if (it == oracle.end()) {
          CHECK(r.value().inserted);
          oracle[k] = l;
        } else {
          CHECK(!r.value().inserted);
          CHECK(r.value().loc == it->second);
        }
        break;
      }
      case 1: {  // erase
        const bool erased = idx.erase(fp_n(k));
        CHECK_EQ(erased, oracle.erase(k) == 1);
        break;
      }
      case 2: {  // repoint
        const Location l = loc_n(static_cast<uint32_t>(rng() % 100000));
        const bool ok = idx.repoint(fp_n(k), l);
        const auto it = oracle.find(k);
        CHECK_EQ(ok, it != oracle.end());
        if (ok) it->second = l;
        break;
      }
      default: {  // lookup
        Location out;
        const bool found = idx.lookup(fp_n(k), &out);
        const auto it = oracle.find(k);
        CHECK_EQ(found, it != oracle.end());
        if (found) CHECK(out == it->second);
        break;
      }
    }
  }
  CHECK_EQ(idx.size(), oracle.size());
  std::printf("  [info] 200k random ops vs std::map oracle: %zu keys survive\n", oracle.size());
}

// GC's safety rule depends on touch_epoch being stamped by BOTH writes and
// dedup hits (SPEC 3.6). If a hit did not stamp, a chunk being actively deduped
// against could be reclaimed mid-cycle.
TEST(index_stamps_epoch_on_write_and_on_hit) {
  Index idx;
  const uint32_t e0 = idx.epoch();
  auto r = idx.insert(fp_n(1), loc_n(1));
  REQUIRE(r.ok());

  uint32_t seen = 0;
  auto capture = [&](const Entry& e) { seen = e.touch_epoch.load(std::memory_order_relaxed); };
  const size_t shard = fp_n(1)[0];
  (void)idx.scan_shard(shard, 0, 1000000, capture);
  CHECK_EQ(seen, e0);

  const uint32_t e1 = idx.bump_epoch();
  CHECK_GT(e1, e0);
  // A dedup hit on an old chunk must move it into the current epoch.
  CHECK(idx.lookup(fp_n(1), nullptr));
  (void)idx.scan_shard(shard, 0, 1000000, capture);
  CHECK_EQ(seen, e1);

  // So must losing an insert race.
  const uint32_t e2 = idx.bump_epoch();
  auto again = idx.insert(fp_n(1), loc_n(2));
  REQUIRE(again.ok());
  CHECK(!again.value().inserted);
  (void)idx.scan_shard(shard, 0, 1000000, capture);
  CHECK_EQ(seen, e2);
}

// S7: GC must be able to walk the index in bounded batches so it never holds a
// lock long enough to stall an ingest thread.
TEST(index_scan_shard_is_resumable_and_bounded) {
  Index idx(8);
  constexpr size_t kN = 20000;
  for (size_t i = 0; i < kN; i++) {
    auto r = idx.insert(fp_n(i), loc_n(static_cast<uint32_t>(i)));
    REQUIRE(r.ok());
  }
  size_t seen = 0;
  size_t batches = 0;
  for (size_t shard = 0; shard < idx.shard_count(); shard++) {
    size_t cursor = 0;
    do {
      cursor = idx.scan_shard(shard, cursor, 16, [&](const Entry&) { seen++; });
      batches++;
      REQUIRE(batches < 1000000);
    } while (cursor != 0);
  }
  CHECK_EQ(seen, kN);
  std::printf("  [info] walked %zu entries in %zu batches of <=16 slots\n", seen, batches);
}

// The 256-way split must actually spread the keys; a skewed shard would
// serialize ingest threads on one mutex and inflate probe distances.
TEST(index_shards_are_evenly_loaded) {
  Index idx(8);
  constexpr size_t kN = 100000;
  for (size_t i = 0; i < kN; i++) {
    auto r = idx.insert(fp_n(i), loc_n(static_cast<uint32_t>(i)));
    REQUIRE(r.ok());
  }
  std::vector<size_t> per_shard(idx.shard_count(), 0);
  for (size_t s = 0; s < idx.shard_count(); s++) {
    size_t cursor = 0;
    do {
      cursor = idx.scan_shard(s, cursor, 4096, [&](const Entry&) { per_shard[s]++; });
    } while (cursor != 0);
  }
  const size_t expect = kN / idx.shard_count();
  size_t lo = kN, hi = 0;
  for (size_t c : per_shard) {
    lo = std::min(lo, c);
    hi = std::max(hi, c);
  }
  std::printf("  [info] %zu keys over %zu shards: expect %zu, min %zu, max %zu\n", kN,
              idx.shard_count(), expect, lo, hi);
  // +-25% around the mean at ~390 keys/shard is a wide but meaningful band:
  // a broken shard function would show up as 0 or kN, not 350.
  CHECK_GT(lo, expect * 3 / 4);
  CHECK_LT(hi, expect * 5 / 4);
}

RUN_ALL()
