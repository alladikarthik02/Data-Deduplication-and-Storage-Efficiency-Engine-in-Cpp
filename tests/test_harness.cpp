// T0 smoke suite.
//
// Two jobs:
//   1. Prove the harness itself works (a test framework whose failures are silent
//      is worse than none).
//   2. Verify the platform assumptions SPEC 3.3 / 3.4 are built on, *before* we
//      write code that depends on them. Every CHECK here corresponds to a sentence
//      in the spec that would otherwise be an assumption.
#include <atomic>
#include <bit>
#include <climits>
#include <cstdint>
#include <tuple>
#include <vector>

#include "test.h"

// ---------------------------------------------------------------------------
// 1. Harness sanity
// ---------------------------------------------------------------------------

TEST(harness_basic_checks_pass) {
  CHECK(true);
  CHECK_EQ(2 + 2, 4);
  CHECK_NE(1, 2);
  CHECK_LT(1, 2);
  CHECK_GE(2, 2);
}

TEST(harness_context_and_loops) {
  // If this ever fails, the TCTX string tells us *which* iteration -- the point
  // of having it at all.
  for (int i = 0; i < 4; i++) {
    TCTX("iter=" << i);
    CHECK_EQ(i * 2, i + i);
  }
}

TEST(harness_seed_is_stable_within_a_run) {
  CHECK_EQ(testing::seed(), testing::seed());
  CHECK_NE(testing::seed(), uint64_t{0});
}

// ---------------------------------------------------------------------------
// 2. Platform assumptions behind the on-disk format (SPEC 3.3, 3.4)
// ---------------------------------------------------------------------------

// SPEC 3.3 declares the on-disk format little-endian and reads multi-byte fields
// by memcpy. On a big-endian host every stored length and offset would be
// byte-swapped garbage. Assert it rather than assume it.
TEST(platform_is_little_endian) {
  CHECK(std::endian::native == std::endian::little);
}

TEST(platform_byte_and_int_widths) {
  CHECK_EQ(CHAR_BIT, 8);
  CHECK_EQ(sizeof(uint64_t), size_t{8});
  CHECK_EQ(sizeof(uint32_t), size_t{4});
}

// SPEC 3.4: the index entry must be exactly 32 bytes -- two per 64-byte cache
// line. If padding silently inflates it to 40, the memory-efficiency claim (R3.1)
// is wrong by 25% and every RAM figure in the spec is wrong with it.
namespace {
struct EntryProto {
  uint64_t k0, k1;
  uint64_t loc;
  uint32_t touch_epoch;
  uint32_t flags;
};
// SPEC 3.3: the chunk record header must be exactly 48 bytes.
struct ChunkHeaderProto {
  uint32_t magic;
  uint32_t length;
  uint32_t crc32c;
  uint32_t flags;
  uint8_t fp[32];
};
}  // namespace

TEST(index_entry_is_32_bytes) {
  CHECK_EQ(sizeof(EntryProto), size_t{32});
  CHECK_EQ(alignof(EntryProto), size_t{8});
  CHECK_EQ(size_t{64} / sizeof(EntryProto), size_t{2});  // 2 entries per cache line
}

TEST(chunk_header_is_48_bytes) {
  CHECK_EQ(sizeof(ChunkHeaderProto), size_t{48});
}

// SPEC 3.4/3.6: the epoch counter and the shard bookkeeping are read on the hot
// path. If atomics on this platform fell back to a lock, the "does not block the
// foreground" claim (S7) would be quietly false.
TEST(atomics_are_lock_free) {
  std::atomic<uint32_t> e32{0};
  std::atomic<uint64_t> e64{0};
  CHECK(e32.is_lock_free());
  CHECK(e64.is_lock_free());
}

// SPEC 3.4 packs container_id:16 | offset:27 | length:17 into one uint64_t.
// Prove the arithmetic works before building an index on top of it.
TEST(location_packing_arithmetic_holds) {
  auto pack = [](uint64_t container, uint64_t offset, uint64_t length) {
    return (container << 44) | (offset << 17) | length;
  };
  auto unpack = [](uint64_t v) {
    return std::tuple<uint64_t, uint64_t, uint64_t>{v >> 44, (v >> 17) & ((1u << 27) - 1),
                                                    v & ((1u << 17) - 1)};
  };
  const uint64_t max_container = (1ull << 16) - 1;   // 65535
  const uint64_t max_offset = (1ull << 27) - 1;      // 128 MiB - 1
  const uint64_t max_length = (1ull << 17) - 1;      // 131071 > 64 KiB max chunk

  for (auto [c, o, l] : std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>{
           {0, 0, 0}, {1, 4096, 8192}, {max_container, max_offset, max_length}}) {
    TCTX("c=" << c << " o=" << o << " l=" << l);
    auto [c2, o2, l2] = unpack(pack(c, o, l));
    CHECK_EQ(c2, c);
    CHECK_EQ(o2, o);
    CHECK_EQ(l2, l);
  }
  // The spec's capacity claim: 65536 containers x 128 MiB = 8 TiB.
  CHECK_EQ((max_container + 1) * (max_offset + 1), uint64_t{8} * 1024 * 1024 * 1024 * 1024);
}

RUN_ALL()
