// Containers: append, read-with-verification, and crash recovery (SPEC S1, S3, S9).
//
// The recovery tests are the reason this file exists. A crash mid-append leaves a
// torn tail, and the engine's claim is that it detects the tear, truncates to the
// last complete record, and keeps every byte written before it.
#include <random>
#include <string>
#include <vector>

#include "dedupe/container.h"
#include "dedupe/crc32c.h"
#include "dedupe/sha256.h"
#include "test.h"
#include "tmpdir.h"

using namespace dedupe;

namespace {

const std::array<uint8_t, 16> kUuid{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

std::vector<uint8_t> payload_of(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(i * 7 + seed);
  return v;
}

struct Written {
  Digest32 fp;
  uint32_t offset;
  std::vector<uint8_t> payload;
};

// Fill a container with `n` chunks and return what went where.
std::vector<Written> fill(const std::string& path, size_t n, size_t chunk_size = 4096) {
  std::vector<Written> out;
  auto w = ContainerWriter::create(path, 7, kUuid);
  if (!w.ok()) return out;
  for (size_t i = 0; i < n; i++) {
    auto p = payload_of(chunk_size, static_cast<uint8_t>(i));
    const Digest32 fp = sha256(p);
    auto off = w.value().append(fp, p);
    if (!off.ok()) break;
    out.push_back({fp, off.value(), std::move(p)});
  }
  (void)w.value().sync();
  (void)w.value().close();
  return out;
}

struct FaultGuard {
  ~FaultGuard() { faults::reset(); }
};

}  // namespace

TEST(container_append_and_read_back) {
  testing::TempDir dir;
  const std::string path = dir.file("c1.dat");
  const auto written = fill(path, 50);
  REQUIRE(written.size() == 50);

  auto r = ContainerReader::open(path);
  REQUIRE(r.ok());
  CHECK_EQ(r.value().id(), uint32_t{7});
  CHECK(r.value().header().store_uuid == kUuid);

  for (size_t i = 0; i < written.size(); i++) {
    TCTX("chunk " << i);
    auto got = r.value().read(written[i].offset,
                              static_cast<uint32_t>(written[i].payload.size()), &written[i].fp);
    REQUIRE(got.ok());
    CHECK(got.value() == written[i].payload);
  }
}

TEST(container_create_refuses_to_overwrite) {
  testing::TempDir dir;
  const std::string path = dir.file("c.dat");
  REQUIRE(fill(path, 1).size() == 1);
  // Re-creating would destroy chunks the index still points at.
  auto again = ContainerWriter::create(path, 7, kUuid);
  CHECK(!again.ok());
  CHECK(again.status().code() == Err::kExists);
}

TEST(container_read_rejects_bad_arguments_and_bad_index_entries) {
  testing::TempDir dir;
  const std::string path = dir.file("c.dat");
  const auto written = fill(path, 5);
  REQUIRE(written.size() == 5);
  auto r = ContainerReader::open(path);
  REQUIRE(r.ok());

  // Length disagreeing with the record's own header -- a stale or corrupt index.
  auto wrong_len = r.value().read(written[0].offset, 999, &written[0].fp);
  CHECK(!wrong_len.ok());
  CHECK(wrong_len.status().code() == Err::kCorrupt);

  // Right record, wrong fingerprint. This is what a truncated-key collision
  // would look like (S8), and it must never return the bytes anyway.
  const Digest32 other = written[1].fp;
  auto wrong_fp = r.value().read(written[0].offset,
                                 static_cast<uint32_t>(written[0].payload.size()), &other);
  CHECK(!wrong_fp.ok());
  CHECK(wrong_fp.status().code() == Err::kCorrupt);

  // An offset past the end of the file must be bounds-checked before the read.
  auto oob = r.value().read(100u << 20, 4096, nullptr);
  CHECK(!oob.ok());
  CHECK(oob.status().code() == Err::kCorrupt);

  CHECK(!r.value().read(written[0].offset, 0, nullptr).ok());
  CHECK(!r.value().read(written[0].offset, kMaxChunkLength + 1, nullptr).ok());
}

// Bit rot in a payload must be caught on read. This is what the per-record CRC
// is for, and it is why restore can afford to verify every chunk (T1: CRC32C is
// 65x faster than SHA-256 in hardware).
TEST(container_read_detects_payload_corruption) {
  testing::TempDir dir;
  const std::string path = dir.file("c.dat");
  const auto written = fill(path, 3);
  REQUIRE(written.size() == 3);

  const uint64_t payload_start = written[1].offset + ChunkHeader::kSize;
  REQUIRE(testing::corrupt_byte(path, payload_start + 100, 0x01));

  auto r = ContainerReader::open(path);
  REQUIRE(r.ok());
  auto got = r.value().read(written[1].offset,
                            static_cast<uint32_t>(written[1].payload.size()), &written[1].fp);
  CHECK(!got.ok());
  CHECK(got.status().code() == Err::kCorrupt);

  // Neighbouring records are unaffected: damage is contained to its record.
  for (size_t i : {size_t{0}, size_t{2}}) {
    TCTX("neighbour " << i);
    auto ok = r.value().read(written[i].offset,
                             static_cast<uint32_t>(written[i].payload.size()), &written[i].fp);
    CHECK(ok.ok());
  }
}

TEST(container_scan_finds_every_record) {
  testing::TempDir dir;
  const std::string path = dir.file("c.dat");
  const auto written = fill(path, 40);
  REQUIRE(written.size() == 40);

  auto r = ContainerReader::open(path);
  REQUIRE(r.ok());
  auto scan = r.value().scan();
  REQUIRE(scan.ok());
  CHECK(!scan.value().torn);
  REQUIRE(scan.value().records.size() == written.size());
  for (size_t i = 0; i < written.size(); i++) {
    TCTX("record " << i);
    CHECK_EQ(scan.value().records[i].offset, written[i].offset);
    CHECK_EQ(scan.value().records[i].length, static_cast<uint32_t>(written[i].payload.size()));
    CHECK(scan.value().records[i].fp == written[i].fp);
  }
}

// ===========================================================================
// S3 -- crash recovery. A torn tail must be detected, and everything before it
// must survive untouched. Truncating at EVERY byte offset inside the last two
// records covers partial headers, partial payloads and exact boundaries without
// having to guess which cases matter.
// ===========================================================================
TEST(container_recovers_from_torn_tail_at_every_offset) {
  testing::TempDir dir;
  const std::string source = dir.file("src.dat");
  const auto written = fill(source, 6, 512);
  REQUIRE(written.size() == 6);

  auto sz = file_size(source);
  REQUIRE(sz.ok());
  const uint64_t full = sz.value();
  const uint64_t last_record_start = written.back().offset;

  for (uint64_t cut = last_record_start; cut <= full; cut++) {
    TCTX("truncated to " << cut << " of " << full);
    const std::string path = dir.file("t" + std::to_string(cut) + ".dat");
    { auto data = read_file(source); REQUIRE(data.ok());
      CHECK(write_file_atomic(path, ByteSpan(data.value().data(), cut)).ok()); }

    auto rec = recover_container(path);
    REQUIRE(rec.ok());
    // Two independent things, which my first version conflated:
    //   `complete`     -- did the last record survive at all? Only when nothing
    //                     was cut.
    //   `expect_torn`  -- is there detectable DAMAGE? A cut exactly at a record
    //                     boundary loses a record without leaving a tear: what
    //                     remains is a well-formed container, indistinguishable
    //                     from one that was never longer.
    const bool complete = (cut == full);
    const bool expect_torn = !complete && (cut != last_record_start);
    const size_t expect_records = complete ? written.size() : written.size() - 1;
    const uint64_t expect_valid = complete ? full : last_record_start;
    CHECK_EQ(rec.value().torn, expect_torn);
    CHECK_EQ(rec.value().records.size(), expect_records);
    CHECK_EQ(rec.value().valid_bytes, expect_valid);

    // After recovery the file contains only whole records, and re-scanning is
    // clean -- recovery is idempotent.
    auto again = recover_container(path);
    REQUIRE(again.ok());
    CHECK(!again.value().torn);
    CHECK_EQ(again.value().records.size(), expect_records);

    // And the recovered container is writable again, right where it left off.
    auto w = ContainerWriter::reopen(path);
    REQUIRE(w.ok());
    auto p = payload_of(300, 99);
    auto off = w.value().append(sha256(p), p);
    REQUIRE(off.ok());
    CHECK_EQ(static_cast<uint64_t>(off.value()), expect_valid);
    CHECK(w.value().sync().ok());
    CHECK(w.value().close().ok());

    auto final_scan = recover_container(path);
    REQUIRE(final_scan.ok());
    CHECK(!final_scan.value().torn);
    CHECK_EQ(final_scan.value().records.size(), expect_records + 1);
  }
}

// A record damaged in the MIDDLE of the file is different from a torn tail: the
// scan stops there and reports every earlier record. It must not try to
// resynchronize by hunting for the next magic -- guessing where a record starts
// is how a scanner turns corruption into fabricated data.
TEST(container_scan_stops_at_mid_file_damage) {
  testing::TempDir dir;
  const std::string path = dir.file("c.dat");
  const auto written = fill(path, 10, 256);
  REQUIRE(written.size() == 10);

  // Corrupt the magic of record 4.
  REQUIRE(testing::corrupt_byte(path, written[4].offset, 0xFF));
  auto r = ContainerReader::open(path);
  REQUIRE(r.ok());
  auto scan = r.value().scan();
  REQUIRE(scan.ok());
  CHECK(scan.value().torn);
  CHECK_EQ(scan.value().records.size(), size_t{4});
  CHECK_EQ(scan.value().valid_bytes, static_cast<uint64_t>(written[4].offset));

  // Corrupting a payload instead is caught by the CRC, at the same record.
  const std::string path2 = dir.file("c2.dat");
  const auto w2 = fill(path2, 10, 256);
  REQUIRE(w2.size() == 10);
  REQUIRE(testing::corrupt_byte(path2, w2[6].offset + ChunkHeader::kSize + 10, 0x01));
  auto r2 = ContainerReader::open(path2);
  REQUIRE(r2.ok());
  auto scan2 = r2.value().scan();
  REQUIRE(scan2.ok());
  CHECK(scan2.value().torn);
  CHECK_EQ(scan2.value().records.size(), size_t{6});
}

TEST(container_rejects_foreign_or_damaged_header) {
  testing::TempDir dir;
  // Not a container at all.
  const std::string junk = dir.file("junk.dat");
  CHECK(write_file_atomic(junk, ByteSpan(reinterpret_cast<const uint8_t*>("not a container!!"), 17))
            .ok());
  CHECK(!ContainerReader::open(junk).ok());

  // Too small to hold a header.
  const std::string tiny = dir.file("tiny.dat");
  CHECK(write_file_atomic(tiny, ByteSpan(reinterpret_cast<const uint8_t*>("x"), 1)).ok());
  auto t = ContainerReader::open(tiny);
  CHECK(!t.ok());
  CHECK(t.status().code() == Err::kCorrupt);

  // Valid container, damaged header.
  const std::string path = dir.file("c.dat");
  REQUIRE(fill(path, 2).size() == 2);
  REQUIRE(testing::corrupt_byte(path, 1, 0xFF));
  CHECK(!ContainerReader::open(path).ok());
}

// A container is bounded at 128 MiB because that is exactly the span of the
// 27-bit offset field in an index entry (SPEC 3.4). Exceeding it would wrap an
// offset silently, so the writer refuses with kNoSpace and the store rolls over.
TEST(container_refuses_to_exceed_its_offset_field) {
  testing::TempDir dir;
  const std::string path = dir.file("big.dat");
  auto w = ContainerWriter::create(path, 1, kUuid);
  REQUIRE(w.ok());

  const auto p = payload_of(kMaxChunkLength, 3);
  const Digest32 fp = sha256(p);
  size_t appended = 0;
  Status last;
  while (true) {
    auto off = w.value().append(fp, p);
    if (!off.ok()) { last = off.status(); break; }
    CHECK_LT(static_cast<uint64_t>(off.value()), kContainerMaxBytes);
    appended++;
    if (appended > 2000) break;  // safety valve; should stop well before this
  }
  CHECK(last.code() == Err::kNoSpace);
  CHECK_LE(w.value().size(), kContainerMaxBytes);
  // "Full" is relative to the chunk being offered: ~83 KB of slack remains, so a
  // max-size chunk no longer fits while a tiny one still would. Asserting
  // !fits(1) was asserting the wrong thing.
  CHECK(!w.value().fits(kMaxChunkLength));
  CHECK(w.value().fits(1));
  std::printf("  [info] container filled with %zu x %u B chunks, %llu bytes\n", appended,
              kMaxChunkLength, static_cast<unsigned long long>(w.value().size()));

  CHECK(!w.value().append(fp, ByteSpan()).ok());  // empty payload
}

// An injected write failure mid-append must surface as an error AND leave a
// container that recovery can still make sense of -- this is the S3/S11 pair.
TEST(container_survives_injected_write_failure) {
  FaultGuard guard;
  testing::TempDir dir;
  const std::string path = dir.file("c.dat");

  auto w = ContainerWriter::create(path, 3, kUuid);
  REQUIRE(w.ok());
  const auto p = payload_of(2048, 5);
  const Digest32 fp = sha256(p);
  for (int i = 0; i < 4; i++) {
    auto off = w.value().append(fp, p);
    REQUIRE(off.ok());
  }
  faults::reset();
  faults::short_nth(faults::Op::kWrite, 1);   // partial record...
  faults::fail_nth(faults::Op::kWrite, 2);    // ...then the retry fails
  auto bad = w.value().append(fp, p);
  CHECK(!bad.ok());
  faults::reset();
  (void)w.value().close();

  // The four complete records are intact; the half-written fifth is a torn tail.
  auto rec = recover_container(path);
  REQUIRE(rec.ok());
  CHECK(rec.value().torn);
  CHECK_EQ(rec.value().records.size(), size_t{4});
}

RUN_ALL()
