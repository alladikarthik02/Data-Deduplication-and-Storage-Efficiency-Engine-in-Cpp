// The store end to end (SPEC R1.3, R1.5, S1, S3, S4, S12, S14).
//
// This is where "restore correctness intact" from the résumé bullet stops being
// a design intention and becomes a property that a randomized round-trip test
// either holds or breaks.
#include <fcntl.h>

#include <random>
#include <string>
#include <vector>

#include "dedupe/container.h"
#include "dedupe/format.h"
#include "dedupe/sha256.h"
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

// Content that actually dedups: a pool of reusable blocks, shuffled. Purely
// random data would dedup at 1.0x and prove nothing about the engine.
std::vector<uint8_t> composed_bytes(std::mt19937_64& rng, size_t n, size_t pool_blocks) {
  std::vector<std::vector<uint8_t>> pool;
  for (size_t i = 0; i < pool_blocks; i++) pool.push_back(random_bytes(rng, 20000));
  std::vector<uint8_t> out;
  while (out.size() < n) {
    const auto& b = pool[rng() % pool.size()];
    out.insert(out.end(), b.begin(), b.end());
  }
  out.resize(n);
  return out;
}

}  // namespace

TEST(store_open_creates_layout) {
  testing::TempDir dir;
  const std::string sp = dir.file("store");
  {
    auto s = Store::open(sp);
    REQUIRE(s.ok());
  }
  for (const char* p : {"STORE", "LOCK", "containers", "recipes"}) {
    TCTX(p);
    auto ex = path_exists(path_join(sp, p));
    REQUIRE(ex.ok());
    CHECK(ex.value());
  }
  // Reopening must find the same store, not create a second one.
  auto again = Store::open(sp);
  REQUIRE(again.ok());
}

// R1.5 -- restore is byte-exact. Every size class that has ever broken a
// chunker: empty, one byte, exactly Min/Avg/Max, one past each, and multi-MiB.
TEST(store_round_trip_is_byte_exact) {
  std::mt19937_64 rng(testing::seed());
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  const std::vector<size_t> sizes = {0,     1,     2,     2047,  2048,   2049,
                                     8191,  8192,  8193,  65535, 65536,  65537,
                                     100000, 1u << 20, (1u << 21) + 12345};
  for (size_t n : sizes) {
    TCTX("size=" << n);
    const auto data = random_bytes(rng, n);
    const std::string name = "rand-" + std::to_string(n);
    auto ing = store.ingest_bytes(name, data);
    REQUIRE(ing.ok());
    CHECK_EQ(ing.value().logical_bytes, static_cast<uint64_t>(n));

    auto got = store.restore_bytes(name);
    REQUIRE(got.ok());
    CHECK_EQ(got.value().size(), n);
    CHECK(got.value() == data);
  }
}

// R1.3 -- a chunk already present is stored once. Ingesting identical content
// under a second name must write essentially nothing.
TEST(store_deduplicates_identical_content) {
  std::mt19937_64 rng(testing::seed() ^ 0x11);
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  const auto data = random_bytes(rng, 4u << 20);
  auto first = store.ingest_bytes("copy-a", data);
  REQUIRE(first.ok());
  CHECK_GT(first.value().chunks_new, uint64_t{0});
  CHECK_EQ(first.value().chunks_deduped, uint64_t{0});

  auto second = store.ingest_bytes("copy-b", data);
  REQUIRE(second.ok());
  CHECK_EQ(second.value().chunks_new, uint64_t{0});
  CHECK_EQ(second.value().physical_bytes, uint64_t{0});
  CHECK_EQ(second.value().chunks_deduped, second.value().chunks_total);

  auto st = store.stats();
  REQUIRE(st.ok());
  std::printf("  [info] 2 copies of 4 MiB: logical %llu, physical %llu, ratio %.2fx\n",
              static_cast<unsigned long long>(st.value().logical_bytes),
              static_cast<unsigned long long>(st.value().physical_bytes),
              st.value().dedup_ratio());
  CHECK_GT(st.value().dedup_ratio(), 1.9);

  // Both names restore to the same bytes.
  for (const char* n : {"copy-a", "copy-b"}) {
    TCTX(n);
    auto got = store.restore_bytes(n);
    REQUIRE(got.ok());
    CHECK(got.value() == data);
  }
}

// Randomized property test over content that actually dedups, with interleaved
// ingests, restores and deletes. The point is that no ORDER of operations
// breaks restore.
TEST(store_property_random_operations) {
  std::mt19937_64 rng(testing::seed() ^ 0x22);
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  std::map<std::string, std::vector<uint8_t>> shadow;
  for (int op = 0; op < 60; op++) {
    TCTX("op=" << op);
    const std::string name = "f" + std::to_string(rng() % 12);
    switch (rng() % 3) {
      case 0:
      case 1: {
        auto data = composed_bytes(rng, 50000 + rng() % 400000, 6);
        auto ing = store.ingest_bytes(name, data);
        REQUIRE(ing.ok());
        shadow[name] = std::move(data);
        break;
      }
      default: {
        if (shadow.count(name)) {
          CHECK(store.remove(name).ok());
          shadow.erase(name);
        }
        break;
      }
    }
    // Everything alive must restore exactly, after every single operation.
    for (const auto& [n, want] : shadow) {
      auto got = store.restore_bytes(n);
      REQUIRE(got.ok());
      if (got.value() != want) {
        TCTX("mismatch on " << n);
        CHECK(false);
      }
    }
  }
  auto ls = store.list();
  REQUIRE(ls.ok());
  CHECK_EQ(ls.value().size(), shadow.size());
}

// SPEC 3.0: containers and recipes are ground truth, the index is a cache. Prove
// it -- close the store, reopen (index rebuilt from a container scan), and
// everything must still restore.
TEST(store_rebuilds_index_from_containers_on_reopen) {
  std::mt19937_64 rng(testing::seed() ^ 0x33);
  testing::TempDir dir;
  const std::string sp = dir.file("store");
  std::map<std::string, std::vector<uint8_t>> want;

  {
    auto opened = Store::open(sp);
    REQUIRE(opened.ok());
    for (int i = 0; i < 6; i++) {
      auto data = composed_bytes(rng, 300000, 5);
      const std::string name = "file" + std::to_string(i);
      REQUIRE(opened.value()->ingest_bytes(name, data).ok());
      want[name] = std::move(data);
    }
  }
  auto reopened = Store::open(sp);
  REQUIRE(reopened.ok());
  CHECK_GT(reopened.value()->index().size(), size_t{0});
  for (const auto& [n, data] : want) {
    TCTX(n);
    auto got = reopened.value()->restore_bytes(n);
    REQUIRE(got.ok());
    CHECK(got.value() == data);
  }
  auto rep = reopened.value()->verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
}

TEST(store_verify_is_clean_and_catches_corruption) {
  std::mt19937_64 rng(testing::seed() ^ 0x44);
  testing::TempDir dir;
  const std::string sp = dir.file("store");
  {
    auto opened = Store::open(sp);
    REQUIRE(opened.ok());
    for (int i = 0; i < 4; i++) {
      REQUIRE(opened.value()->ingest_bytes("f" + std::to_string(i),
                                           composed_bytes(rng, 200000, 4))
                  .ok());
    }
    auto rep = opened.value()->verify(VerifyLevel::kDeep);
    REQUIRE(rep.ok());
    CHECK(rep.value().ok());
    CHECK_GT(rep.value().bytes_checked, uint64_t{0});
  }

  // Flip a byte deep inside a container payload and re-verify.
  auto cnames = list_dir(path_join(sp, "containers"));
  REQUIRE(cnames.ok());
  REQUIRE(!cnames.value().empty());
  const std::string cpath = path_join(path_join(sp, "containers"), cnames.value()[0]);
  REQUIRE(testing::corrupt_byte(cpath, ContainerHeader::kSize + ChunkHeader::kSize + 500, 0x01));

  auto reopened = Store::open(sp);
  REQUIRE(reopened.ok());
  auto rep = reopened.value()->verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(!rep.value().ok());
  std::printf("  [info] corrupted 1 byte -> verify --deep reports %zu problem(s)\n",
              rep.value().problems.size());
}

// S12 -- ingest never modifies the user's data, and a hostile name cannot escape
// the store directory. Names are hashed to produce recipe filenames, so these
// are structurally impossible rather than filtered.
TEST(store_is_safe_with_hostile_names_and_never_touches_the_source) {
  std::mt19937_64 rng(testing::seed() ^ 0x55);
  testing::TempDir dir;
  const std::string sp = dir.file("store");
  auto opened = Store::open(sp);
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  const std::vector<std::string> names = {
      "../../etc/passwd", "/absolute/path", "..", ".", "with/slashes/inside",
      std::string(4000, 'x'), "with spaces and 'quotes'", "unicode-\xc3\xa9\xc3\xa8",
  };
  for (const std::string& n : names) {
    TCTX("name len=" << n.size());
    const auto data = random_bytes(rng, 5000);
    auto ing = store.ingest_bytes(n, data);
    REQUIRE(ing.ok());
    auto got = store.restore_bytes(n);
    REQUIRE(got.ok());
    CHECK(got.value() == data);
  }
  // Nothing escaped: every recipe is a 64-hex-char filename inside recipes/.
  auto listing = list_dir(path_join(sp, "recipes"));
  REQUIRE(listing.ok());
  CHECK_EQ(listing.value().size(), names.size());
  for (const std::string& f : listing.value()) {
    TCTX(f);
    CHECK_EQ(f.size(), size_t{68});  // 64 hex + ".rcp"
    CHECK(f.find('/') == std::string::npos);
  }

  // And the source file is untouched by ingest.
  const std::string src = dir.file("source.bin");
  const auto payload = random_bytes(rng, 100000);
  CHECK(write_file_atomic(src, payload).ok());
  REQUIRE(store.ingest_file("src", src).ok());
  auto after = read_file(src);
  REQUIRE(after.ok());
  CHECK(after.value() == payload);
}

// S14 -- two Store objects must never both hold the same directory.
TEST(store_lock_is_exclusive) {
  testing::TempDir dir;
  const std::string sp = dir.file("store");
  auto a = Store::open(sp);
  REQUIRE(a.ok());
  auto b = Store::open(sp);
  CHECK(!b.ok());
  CHECK(b.status().code() == Err::kLocked);
  a.value().reset();  // release
  auto c = Store::open(sp);
  CHECK(c.ok());
}

// S3 -- a torn container tail from a crash is recovered on open, and the files
// that did not depend on the lost chunks still restore.
TEST(store_recovers_from_a_torn_container) {
  std::mt19937_64 rng(testing::seed() ^ 0x66);
  testing::TempDir dir;
  const std::string sp = dir.file("store");
  std::vector<uint8_t> first_data;
  {
    auto opened = Store::open(sp);
    REQUIRE(opened.ok());
    first_data = random_bytes(rng, 300000);
    REQUIRE(opened.value()->ingest_bytes("early", first_data).ok());
  }
  // Simulate a crash mid-append: glue extra garbage onto the container's tail.
  auto cnames = list_dir(path_join(sp, "containers"));
  REQUIRE(cnames.ok());
  REQUIRE(!cnames.value().empty());
  const std::string cpath = path_join(path_join(sp, "containers"), cnames.value()[0]);
  auto sz = file_size(cpath);
  REQUIRE(sz.ok());
  {
    auto f = File::open(cpath, O_WRONLY | O_APPEND);
    REQUIRE(f.ok());
    const std::vector<uint8_t> junk(1234, 0xAB);
    CHECK(f.value().write_all(junk).ok());
    CHECK(f.value().close().ok());
  }

  auto reopened = Store::open(sp);
  REQUIRE(reopened.ok());
  // The torn tail was truncated away...
  auto sz2 = file_size(cpath);
  REQUIRE(sz2.ok());
  CHECK_EQ(sz2.value(), sz.value());
  // ...and everything written before the tear is intact.
  auto got = reopened.value()->restore_bytes("early");
  REQUIRE(got.ok());
  CHECK(got.value() == first_data);
  auto rep = reopened.value()->verify(VerifyLevel::kDeep);
  REQUIRE(rep.ok());
  CHECK(rep.value().ok());
}

// S4 -- the recipe is published only after its chunks are durable. Fail the
// container's fsync and the file must not appear at all: a file that exists but
// cannot be read is strictly worse than one that was never created.
TEST(store_does_not_publish_a_recipe_when_the_container_sync_fails) {
  testing::FaultGuard guard;
  std::mt19937_64 rng(testing::seed() ^ 0x77);
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  const auto data = random_bytes(rng, 200000);
  faults::reset();
  faults::fail_nth(faults::Op::kFsync, 1);
  auto ing = store.ingest_bytes("doomed", data);
  faults::reset();
  CHECK(!ing.ok());

  auto ex = store.exists("doomed");
  REQUIRE(ex.ok());
  CHECK(!ex.value());
  // The store is still usable afterwards.
  REQUIRE(store.ingest_bytes("fine", data).ok());
  auto got = store.restore_bytes("fine");
  REQUIRE(got.ok());
  CHECK(got.value() == data);
}

TEST(store_remove_and_list) {
  std::mt19937_64 rng(testing::seed() ^ 0x88);
  testing::TempDir dir;
  auto opened = Store::open(dir.file("store"));
  REQUIRE(opened.ok());
  Store& store = *opened.value();

  for (int i = 0; i < 5; i++) {
    REQUIRE(store.ingest_bytes("f" + std::to_string(i), random_bytes(rng, 30000)).ok());
  }
  auto ls = store.list();
  REQUIRE(ls.ok());
  CHECK_EQ(ls.value().size(), size_t{5});
  CHECK_EQ(ls.value()[0].name, std::string("f0"));  // sorted

  CHECK(store.remove("f2").ok());
  CHECK(!store.remove("f2").ok());  // gone
  CHECK(store.remove("never-existed").code() == Err::kNotFound);  // remove returns Status, not Result
  ls = store.list();
  REQUIRE(ls.ok());
  CHECK_EQ(ls.value().size(), size_t{4});

  auto gone = store.restore_bytes("f2");
  CHECK(!gone.ok());
  CHECK(gone.status().code() == Err::kNotFound);

  // Deleting a recipe frees no space until GC runs -- that is the contract.
  auto st = store.stats();
  REQUIRE(st.ok());
  CHECK_GT(st.value().physical_bytes, uint64_t{0});
}

RUN_ALL()
