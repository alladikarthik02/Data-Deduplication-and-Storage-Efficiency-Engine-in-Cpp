// On-disk format encoders/decoders (SPEC S9, S11).
//
// Every decoder here parses UNTRUSTED bytes. The tests are therefore weighted
// towards hostile and damaged input rather than round trips: a round trip proves
// the encoder and decoder agree with each other, which is exactly the property a
// corrupted file does not have.
#include <random>
#include <string>
#include <vector>

#include "dedupe/crc32c.h"
#include "dedupe/format.h"
#include "dedupe/sha256.h"
#include "test.h"

using namespace dedupe;

namespace {

Digest32 fp_of(const std::string& s) { return sha256(as_bytes(std::string_view(s))); }

Recipe make_recipe(size_t n, const std::string& name = "file.bin") {
  Recipe r;
  r.name = name;
  r.file_sha256 = fp_of(name);
  for (size_t i = 0; i < n; i++) {
    RecipeEntry e;
    e.fp = fp_of("chunk" + std::to_string(i));
    e.length = static_cast<uint32_t>(1000 + i);
    r.entries.push_back(e);
    r.logical_size += e.length;
  }
  return r;
}

}  // namespace

// --- ChunkHeader ------------------------------------------------------------

TEST(chunk_header_round_trip) {
  ChunkHeader h;
  h.length = 8192;
  h.crc32c = 0xDEADBEEF;
  h.flags = 0;
  h.fp = fp_of("payload");

  uint8_t buf[ChunkHeader::kSize];
  h.encode(buf);
  auto got = ChunkHeader::decode(ByteSpan(buf, sizeof(buf)));
  REQUIRE(got.ok());
  CHECK_EQ(got.value().length, h.length);
  CHECK_EQ(got.value().crc32c, h.crc32c);
  CHECK(got.value().fp == h.fp);
}

TEST(chunk_header_rejects_damage) {
  ChunkHeader h;
  h.length = 8192;
  h.fp = fp_of("x");
  uint8_t buf[ChunkHeader::kSize];
  h.encode(buf);

  // Truncated: every prefix short of the full header must be refused.
  for (size_t n = 0; n < ChunkHeader::kSize; n++) {
    TCTX("truncated to " << n);
    CHECK(!ChunkHeader::decode(ByteSpan(buf, n)).ok());
  }
  // Bad magic: this is what a torn tail or a random file looks like.
  {
    uint8_t bad[ChunkHeader::kSize];
    std::memcpy(bad, buf, sizeof(bad));
    bad[0] ^= 0xFF;
    auto r = ChunkHeader::decode(ByteSpan(bad, sizeof(bad)));
    CHECK(!r.ok());
    CHECK(r.status().code() == Err::kCorrupt);
  }
  // Zero length: would produce a zero-byte chunk and an infinite scan loop.
  {
    uint8_t bad[ChunkHeader::kSize];
    std::memcpy(bad, buf, sizeof(bad));
    put_u32le(bad + 4, 0);
    CHECK(!ChunkHeader::decode(ByteSpan(bad, sizeof(bad))).ok());
  }
  // Oversize length: the dangerous one. A flipped byte here would otherwise size
  // a read or a buffer. Bound is 17 bits, the width of the index's length field.
  for (uint32_t len : {kMaxChunkLength + 1, uint32_t{1} << 20, uint32_t{0xFFFFFFFF}}) {
    TCTX("length=" << len);
    uint8_t bad[ChunkHeader::kSize];
    std::memcpy(bad, buf, sizeof(bad));
    put_u32le(bad + 4, len);
    auto r = ChunkHeader::decode(ByteSpan(bad, sizeof(bad)));
    CHECK(!r.ok());
    CHECK(r.status().code() == Err::kCorrupt);
  }
  CHECK(ChunkHeader::decode(ByteSpan(buf, sizeof(buf))).ok());  // still valid untouched
}

// --- ContainerHeader --------------------------------------------------------

TEST(container_header_round_trip_and_damage) {
  ContainerHeader h;
  h.container_id = 42;
  for (size_t i = 0; i < 16; i++) h.store_uuid[i] = static_cast<uint8_t>(i);
  uint8_t buf[ContainerHeader::kSize];
  h.encode(buf);

  auto got = ContainerHeader::decode(ByteSpan(buf, sizeof(buf)));
  REQUIRE(got.ok());
  CHECK_EQ(got.value().container_id, uint32_t{42});
  CHECK(got.value().store_uuid == h.store_uuid);

  uint8_t bad[ContainerHeader::kSize];
  std::memcpy(bad, buf, sizeof(bad));
  bad[0] ^= 0xFF;
  CHECK(!ContainerHeader::decode(ByteSpan(bad, sizeof(bad))).ok());

  // An unknown version must be refused, never guessed at. Reading a v2 file with
  // v1 field offsets would produce plausible garbage.
  std::memcpy(bad, buf, sizeof(bad));
  put_u32le(bad + 4, 99);
  auto v = ContainerHeader::decode(ByteSpan(bad, sizeof(bad)));
  CHECK(!v.ok());
  CHECK(v.status().code() == Err::kCorrupt);
}

// --- Recipe -----------------------------------------------------------------

TEST(recipe_round_trip) {
  for (size_t n : {size_t{0}, size_t{1}, size_t{2}, size_t{1000}}) {
    TCTX("entries=" << n);
    const Recipe r = make_recipe(n, "some/path/like/name.bin");
    const auto encoded = r.encode();
    CHECK_EQ(encoded.size(), Recipe::kHeaderSize + r.name.size() + n * Recipe::kEntrySize);

    auto got = Recipe::decode(encoded);
    REQUIRE(got.ok());
    CHECK_EQ(got.value().name, r.name);
    CHECK_EQ(got.value().logical_size, r.logical_size);
    CHECK(got.value().file_sha256 == r.file_sha256);
    REQUIRE(got.value().entries.size() == n);
    for (size_t i = 0; i < n; i++) {
      CHECK(got.value().entries[i].fp == r.entries[i].fp);
      CHECK_EQ(got.value().entries[i].length, r.entries[i].length);
    }
  }
}

TEST(recipe_rejects_truncation_and_bad_magic) {
  const auto encoded = make_recipe(10).encode();
  for (size_t n = 0; n < encoded.size(); n++) {
    TCTX("truncated to " << n);
    // Every proper prefix must be refused: a truncated recipe is what a crash
    // mid-write would leave if rename were not atomic.
    CHECK(!Recipe::decode(ByteSpan(encoded.data(), n)).ok());
  }
  auto bad = encoded;
  bad[0] ^= 0xFF;
  CHECK(!Recipe::decode(bad).ok());
  bad = encoded;
  put_u32le(bad.data() + 4, 7);  // unknown version
  CHECK(!Recipe::decode(bad).ok());
}

// Bit rot anywhere in the body must be caught by the CRC. Walk every byte of a
// small recipe and flip it -- there is no "harmless" byte in this format.
TEST(recipe_crc_catches_every_single_byte_flip) {
  const auto encoded = make_recipe(5).encode();
  size_t caught = 0;
  for (size_t i = 0; i < encoded.size(); i++) {
    auto bad = encoded;
    bad[i] ^= 0x01;
    if (!Recipe::decode(bad).ok()) caught++;
  }
  CHECK_EQ(caught, encoded.size());
}

// THE hostile-input test (SPEC S9). chunk_count comes off the disk and feeds an
// allocation. A single flipped byte can turn 5 into 2^60. If decode() sized a
// vector from it before checking, this test would OOM or abort the process --
// which is exactly what it is here to prevent.
TEST(recipe_rejects_absurd_chunk_count_without_allocating) {
  auto encoded = make_recipe(5).encode();
  for (uint64_t count : {uint64_t{1} << 60, uint64_t{0xFFFFFFFFFFFFFFFFull}, uint64_t{1} << 32,
                         uint64_t{1000000}}) {
    TCTX("chunk_count=" << count);
    auto bad = encoded;
    put_u64le(bad.data() + 16, count);
    auto r = Recipe::decode(bad);
    CHECK(!r.ok());
    CHECK(r.status().code() == Err::kCorrupt);
  }
  // Same for name_len, which also sizes a copy.
  for (uint32_t nl : {uint32_t{0xFFFFFFFF}, uint32_t{1} << 24}) {
    TCTX("name_len=" << nl);
    auto bad = encoded;
    put_u32le(bad.data() + 56, nl);
    auto r = Recipe::decode(bad);
    CHECK(!r.ok());
    CHECK(r.status().code() == Err::kCorrupt);
  }
}

// A recipe can be internally inconsistent without being corrupt in transit: the
// CRC passes because the writer computed it over wrong data. Catching that
// distinguishes "damaged on disk" from "written by a buggy engine", and both
// must be refused.
TEST(recipe_rejects_internally_inconsistent_content) {
  Recipe r = make_recipe(4);
  r.logical_size += 1;  // no longer the sum of the chunk lengths
  auto got = Recipe::decode(r.encode());
  CHECK(!got.ok());
  CHECK(got.status().code() == Err::kCorrupt);

  Recipe z = make_recipe(3);
  z.entries[1].length = 0;  // zero-length chunk
  z.logical_size = z.total_chunk_bytes();
  CHECK(!Recipe::decode(z.encode()).ok());

  Recipe big = make_recipe(3);
  big.entries[2].length = kMaxChunkLength + 1;
  big.logical_size = big.total_chunk_bytes();
  CHECK(!Recipe::decode(big.encode()).ok());
}

// Random mutation, the cheap fuzzer. Every mutated recipe must either decode to
// something self-consistent or be refused -- never crash, never read out of
// bounds. Run under ASan this is the whole S9 claim.
TEST(recipe_fuzz_random_mutations) {
  std::mt19937_64 rng(testing::seed());
  const auto original = make_recipe(20).encode();
  size_t accepted = 0;
  for (int trial = 0; trial < 3000; trial++) {
    auto bad = original;
    const int mutations = 1 + static_cast<int>(rng() % 4);
    for (int m = 0; m < mutations; m++) {
      bad[rng() % bad.size()] = static_cast<uint8_t>(rng() & 0xFF);
    }
    if (rng() % 4 == 0 && bad.size() > 1) bad.resize(rng() % bad.size());
    auto r = Recipe::decode(bad);
    if (r.ok()) {
      accepted++;
      // If it decoded, it must be self-consistent -- that is what "accepted"
      // has to mean for the restore path to be able to trust it.
      CHECK_EQ(r.value().total_chunk_bytes(), r.value().logical_size);
    }
  }
  std::printf("  [info] 3000 mutated recipes: %zu accepted (all self-consistent)\n", accepted);
}

RUN_ALL()
