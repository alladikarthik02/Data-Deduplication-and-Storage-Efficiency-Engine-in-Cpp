// Content-defined chunking (SPEC R1.1, 8.8).
//
// The headline test here is chunker_boundary_shift_property. Everything the
// résumé bullet claims about "content-defined chunks" reduces to that one
// measurement, and it is run side-by-side against fixed-size chunking so the
// difference is a number rather than an assertion of principle.
#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "dedupe/chunker.h"
#include "dedupe/sha256.h"
#include "dedupe/types.h"
#include "test.h"

using namespace dedupe;

namespace {

std::vector<uint8_t> random_bytes(std::mt19937_64& rng, size_t n) {
  std::vector<uint8_t> v(n);
  size_t i = 0;
  // Fill 8 bytes at a time: at 8 MiB the byte-at-a-time version dominates the
  // test's runtime under the sanitizer builds.
  for (; i + 8 <= n; i += 8) {
    const uint64_t r = rng();
    std::memcpy(v.data() + i, &r, 8);
  }
  for (; i < n; i++) v[i] = static_cast<uint8_t>(rng() & 0xFF);
  return v;
}

struct Chunk {
  size_t offset;
  size_t length;
};

// Chunk a whole buffer by repeatedly asking for the next cut point.
std::vector<Chunk> chunk_all(ByteSpan data, const ChunkerParams& p) {
  std::vector<Chunk> out;
  size_t pos = 0;
  while (pos < data.size()) {
    const size_t len = find_cut_point(ByteSpan(data.data() + pos, data.size() - pos), p);
    out.push_back({pos, len});
    pos += len;
  }
  return out;
}

std::vector<Chunk> chunk_all_fixed(ByteSpan data, size_t size) {
  std::vector<Chunk> out;
  for (size_t pos = 0; pos < data.size(); pos += size) {
    out.push_back({pos, std::min(size, data.size() - pos)});
  }
  return out;
}

// The set of chunk CONTENTS (by fingerprint), which is what a dedup store keys
// on. Comparing offsets would answer the wrong question entirely -- after an
// insertion every offset shifts, yet the content is what dedups.
std::set<std::string> fingerprints(ByteSpan data, const std::vector<Chunk>& chunks) {
  std::set<std::string> fps;
  for (const auto& c : chunks) {
    fps.insert(to_hex(sha256(ByteSpan(data.data() + c.offset, c.length))));
  }
  return fps;
}

double shared_fraction(const std::set<std::string>& a, const std::set<std::string>& b) {
  size_t shared = 0;
  for (const auto& fp : b) {
    if (a.count(fp)) shared++;
  }
  return b.empty() ? 0.0 : static_cast<double>(shared) / static_cast<double>(b.size());
}

}  // namespace

TEST(chunker_params_validate) {
  CHECK(ChunkerParams{}.valid());
  ChunkerParams p;
  p.min_size = 0;
  CHECK(!p.valid());
  p = {}; p.min_size = p.avg_size;            CHECK(!p.valid());
  p = {}; p.max_size = p.avg_size;            CHECK(!p.valid());
  p = {}; p.mask_s = 0;                       CHECK(!p.valid());
  // The one that is a corruption bug, not a taste question: a chunk longer than
  // 131071 bytes does not fit the 17-bit length field of an index entry.
  p = {}; p.max_size = 131072;                CHECK(!p.valid());
  p = {}; p.max_size = 131071;                CHECK(p.valid());
}

// Every chunk must be within [min, max], except the last, which is whatever is
// left over. Violating the max bound would silently corrupt the index (17-bit
// length field); violating the min bound wastes metadata on every chunk.
TEST(chunker_respects_min_and_max) {
  std::mt19937_64 rng(testing::seed());
  const ChunkerParams p;
  const auto data = random_bytes(rng, 4u << 20);
  const auto chunks = chunk_all(data, p);
  REQUIRE(chunks.size() > 100);
  for (size_t i = 0; i < chunks.size(); i++) {
    TCTX("chunk " << i << "/" << chunks.size() << " len=" << chunks[i].length);
    CHECK_LE(chunks[i].length, size_t{p.max_size});
    if (i + 1 < chunks.size()) CHECK_GE(chunks[i].length, size_t{p.min_size});
    CHECK_GT(chunks[i].length, size_t{0});  // a zero-length chunk would loop forever
  }
}

// Lossless: the chunks tile the input exactly, with no gap and no overlap.
// Everything downstream (restore correctness, S1) depends on this.
TEST(chunker_is_complete_and_lossless) {
  std::mt19937_64 rng(testing::seed() ^ 0x11);
  for (size_t n : {size_t{0}, size_t{1}, size_t{1000}, size_t{100000}, size_t{1u << 20}}) {
    TCTX("n=" << n);
    const auto data = random_bytes(rng, n);
    const auto chunks = chunk_all(data, ChunkerParams{});
    size_t total = 0, expect_offset = 0;
    for (const auto& c : chunks) {
      CHECK_EQ(c.offset, expect_offset);
      expect_offset += c.length;
      total += c.length;
    }
    CHECK_EQ(total, n);
  }
}

TEST(chunker_is_deterministic) {
  std::mt19937_64 rng(testing::seed() ^ 0x22);
  const auto data = random_bytes(rng, 1u << 20);
  const auto a = chunk_all(data, ChunkerParams{});
  const auto b = chunk_all(data, ChunkerParams{});
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); i++) {
    TCTX("i=" << i);
    CHECK_EQ(a[i].offset, b[i].offset);
    CHECK_EQ(a[i].length, b[i].length);
  }
}

// Streaming must equal whole-buffer chunking for EVERY feed pattern. This is the
// same class of bug as SHA-256's incremental path: the "may I cut yet?" rule in
// Chunker::drain is only exercised when data arrives in pieces, so it can stay
// broken until the first file larger than one read buffer.
TEST(chunker_streaming_equals_whole_buffer) {
  std::mt19937_64 rng(testing::seed() ^ 0x33);
  const auto data = random_bytes(rng, 2u << 20);
  const auto want = chunk_all(data, ChunkerParams{});

  for (int trial = 0; trial < 8; trial++) {
    TCTX("trial=" << trial);
    std::vector<Chunk> got;
    size_t emitted = 0;
    Chunker ch;
    auto emit = [&](ByteSpan c) {
      got.push_back({emitted, c.size()});
      // Also verify the bytes themselves, not just the lengths: a chunker that
      // emitted correct lengths from the wrong offsets would pass a
      // length-only check and corrupt every restore.
      CHECK(std::equal(c.begin(), c.end(), data.begin() + static_cast<long>(emitted)));
      emitted += c.size();
    };

    size_t pos = 0;
    while (pos < data.size()) {
      // Deliberately awkward feed sizes: 1 byte, sub-min, around max, huge.
      static const size_t kSizes[] = {1, 7, 999, 2048, 65535, 65536, 65537, 300000};
      size_t take = kSizes[rng() % (sizeof(kSizes) / sizeof(kSizes[0]))];
      take = std::min(take, data.size() - pos);
      ch.feed(ByteSpan(data.data() + pos, take), emit);
      pos += take;
    }
    ch.finish(emit);

    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < want.size(); i++) {
      TCTX("chunk=" << i);
      CHECK_EQ(got[i].offset, want[i].offset);
      CHECK_EQ(got[i].length, want[i].length);
    }
  }
}

// ===========================================================================
// R1.1 -- the property the whole project rests on.
// ===========================================================================
TEST(chunker_boundary_shift_property) {
  std::mt19937_64 rng(testing::seed() ^ 0x44);
  const auto original = random_bytes(rng, 8u << 20);

  struct Edit {
    const char* name;
    size_t at;
  };
  const Edit edits[] = {
      {"insert 1 byte at offset 0", 0},
      {"insert 1 byte at 25%", original.size() / 4},
      {"insert 1 byte in the middle", original.size() / 2},
  };

  const ChunkerParams p;
  const auto cdc_orig = chunk_all(original, p);
  const auto cdc_orig_fps = fingerprints(original, cdc_orig);
  const auto fixed_orig = chunk_all_fixed(original, p.avg_size);
  const auto fixed_orig_fps = fingerprints(original, fixed_orig);

  std::printf("  [info] corpus %zu MiB -> %zu CDC chunks, %zu fixed chunks\n",
              original.size() >> 20, cdc_orig.size(), fixed_orig.size());

  for (const auto& e : edits) {
    TCTX(e.name);
    std::vector<uint8_t> edited = original;
    edited.insert(edited.begin() + static_cast<long>(e.at), uint8_t{0x42});

    const double cdc_kept =
        shared_fraction(cdc_orig_fps, fingerprints(edited, chunk_all(edited, p)));
    const double fixed_kept = shared_fraction(
        fixed_orig_fps, fingerprints(edited, chunk_all_fixed(edited, p.avg_size)));

    std::printf("  [info] %-28s  CDC keeps %5.1f%%   fixed-size keeps %5.1f%%\n", e.name,
                cdc_kept * 100, fixed_kept * 100);

    // The claim: content-defined chunking re-synchronizes after an edit.
    CHECK_GE(cdc_kept, 0.90);

    // The contrast that makes the claim meaningful. An insertion at offset 0
    // shifts every fixed-size block, so essentially nothing is preserved. Any
    // chunk before the edit point still matches, which is why this is not
    // asserted to be exactly zero for mid-file edits.
    if (e.at == 0) CHECK_LT(fixed_kept, 0.01);
  }
}

// The realistic version of the same property: a file edited in several places,
// as a backup workload actually sees. This previews the dedup ratio measured
// for real in T9.
TEST(chunker_dedup_across_realistic_edits) {
  std::mt19937_64 rng(testing::seed() ^ 0x55);
  const auto v1 = random_bytes(rng, 4u << 20);
  std::vector<uint8_t> v2 = v1;

  // 20 scattered edits: insertions, deletions and overwrites.
  for (int i = 0; i < 20; i++) {
    const size_t at = rng() % v2.size();
    switch (rng() % 3) {
      case 0: v2.insert(v2.begin() + static_cast<long>(at), 32, uint8_t{0xAA}); break;
      case 1:
        v2.erase(v2.begin() + static_cast<long>(at),
                 v2.begin() + static_cast<long>(std::min(at + 32, v2.size())));
        break;
      default: v2[at] ^= 0xFF; break;
    }
  }

  const ChunkerParams p;
  const auto fps1 = fingerprints(v1, chunk_all(v1, p));
  const auto fps2 = fingerprints(v2, chunk_all(v2, p));
  const double kept = shared_fraction(fps1, fps2);
  std::printf("  [info] 20 scattered edits in 4 MiB: %.1f%% of chunks still dedup\n",
              kept * 100);
  // 20 edits can each destroy at most a couple of chunks out of ~512.
  CHECK_GE(kept, 0.90);
}

// SPEC 8.8: every RAM and metadata-overhead figure in the spec assumes the
// chunker actually hits its 8 KiB target. Rolling-hash chunkers routinely miss
// it. Measure and report, don't assume.
TEST(chunker_size_distribution) {
  std::mt19937_64 rng(testing::seed() ^ 0x66);
  const ChunkerParams p;
  const auto data = random_bytes(rng, 8u << 20);
  const auto chunks = chunk_all(data, p);
  REQUIRE(chunks.size() > 500);

  std::vector<size_t> lens;
  lens.reserve(chunks.size());
  size_t at_max = 0, at_min = 0;
  for (size_t i = 0; i < chunks.size(); i++) {
    if (i + 1 == chunks.size()) continue;  // ignore the ragged tail chunk
    lens.push_back(chunks[i].length);
    if (chunks[i].length == p.max_size) at_max++;
    if (chunks[i].length == p.min_size) at_min++;
  }
  std::sort(lens.begin(), lens.end());
  const auto pct = [&](double q) { return lens[static_cast<size_t>(q * (lens.size() - 1))]; };
  double mean = 0;
  for (size_t l : lens) mean += static_cast<double>(l);
  mean /= static_cast<double>(lens.size());

  std::printf(
      "  [info] n=%zu  mean=%.0f B  p1=%zu  p50=%zu  p99=%zu  max=%zu  at_max=%.2f%%  "
      "at_min=%.2f%%\n",
      lens.size(), mean, pct(0.01), pct(0.50), pct(0.99), lens.back(),
      100.0 * static_cast<double>(at_max) / static_cast<double>(lens.size()),
      100.0 * static_cast<double>(at_min) / static_cast<double>(lens.size()));

  // The target is 8192. Allow a generous band -- this is a statistical property
  // of a hash over random data, not an exact quantity -- but a mean outside it
  // means the masks or the normalization are wrong and every overhead figure in
  // SPEC 3.4 is wrong with them.
  CHECK_GT(mean, 6000.0);
  CHECK_LT(mean, 11000.0);
}

TEST(chunker_edge_cases) {
  const ChunkerParams p;
  std::mt19937_64 rng(testing::seed() ^ 0x77);
  for (size_t n : {size_t{0}, size_t{1}, size_t{2047}, size_t{2048}, size_t{2049},
                   size_t{8191}, size_t{8192}, size_t{8193}, size_t{65535}, size_t{65536},
                   size_t{65537}, size_t{131071}}) {
    TCTX("n=" << n);
    const auto data = random_bytes(rng, n);
    const size_t len = find_cut_point(data, p);
    CHECK_LE(len, n);
    CHECK_LE(len, size_t{p.max_size});
    if (n > 0) CHECK_GT(len, size_t{0});
    if (n <= p.min_size) CHECK_EQ(len, n);  // too short to cut
  }
}

// Pathological content: long runs of one byte give the gear hash no entropy, so
// no cut point is ever found and every chunk is forced out at MaxSize. That is
// correct behaviour -- but it must not hang, produce zero-length chunks, or
// exceed the max.
TEST(chunker_pathological_inputs) {
  const ChunkerParams p;
  for (auto [name, fill] : std::vector<std::pair<const char*, uint8_t>>{
           {"all zeros", 0x00}, {"all 0xFF", 0xFF}}) {
    TCTX(name);
    const std::vector<uint8_t> data(1u << 20, fill);
    const auto chunks = chunk_all(data, p);
    size_t total = 0;
    for (const auto& c : chunks) {
      CHECK_GT(c.length, size_t{0});
      CHECK_LE(c.length, size_t{p.max_size});
      total += c.length;
    }
    CHECK_EQ(total, data.size());
    std::printf("  [info] %-10s -> %zu chunks (all forced at MaxSize means %zu)\n", name,
                chunks.size(), (data.size() + p.max_size - 1) / p.max_size);
  }

  // A short repeating pattern: some entropy, but highly structured. A chunker
  // that cut on every repetition would produce min-size chunks forever.
  std::vector<uint8_t> pattern(1u << 20);
  for (size_t i = 0; i < pattern.size(); i++) pattern[i] = static_cast<uint8_t>(i % 251);
  const auto chunks = chunk_all(pattern, p);
  size_t total = 0;
  for (const auto& c : chunks) {
    CHECK_GT(c.length, size_t{0});
    CHECK_LE(c.length, size_t{p.max_size});
    total += c.length;
  }
  CHECK_EQ(total, pattern.size());
  std::printf("  [info] repeating pattern -> %zu chunks, mean %.0f B\n", chunks.size(),
              static_cast<double>(pattern.size()) / static_cast<double>(chunks.size()));
}

RUN_ALL()
