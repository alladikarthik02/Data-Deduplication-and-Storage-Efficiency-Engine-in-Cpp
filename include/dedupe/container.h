// Containers: append-only files of self-describing chunk records.
//
// GROUND TRUTH LIVES HERE (SPEC 3.0). Containers plus recipes are the only
// authoritative state; the in-memory index is a cache that can be rebuilt by
// scanning these files. That is what turns index durability from a correctness
// problem into a performance problem, and it is why every record carries its own
// fingerprint, length and CRC rather than relying on an external directory.
//
// CRASH MODEL (S3). A crash mid-append leaves a torn tail: a partial header, or
// a complete header whose payload never landed. scan() detects that -- bad magic,
// out-of-range length, short payload, or failed CRC -- and reports how many bytes
// are valid. recover() truncates to that point. Everything before the tear is
// untouched, because the file is append-only and nothing rewrites it in place.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "dedupe/format.h"
#include "dedupe/io.h"
#include "dedupe/status.h"
#include "dedupe/types.h"

namespace dedupe {

// 128 MiB: exactly the span of the 27-bit offset field in an index entry
// (SPEC 3.4). Not a tuning knob -- exceeding it would silently wrap an offset.
inline constexpr uint64_t kContainerMaxBytes = 1ull << 27;

struct ScanRecord {
  Digest32 fp{};
  uint32_t offset = 0;  // absolute offset of the record HEADER in the file
  uint32_t length = 0;  // payload bytes
};

struct ScanResult {
  ContainerHeader header;
  std::vector<ScanRecord> records;
  uint64_t valid_bytes = 0;  // everything before this offset parsed cleanly
  bool torn = false;         // true if the file continues past valid_bytes
  std::string tear_reason;
};

class ContainerWriter {
 public:
  ContainerWriter() = default;

  // Create a new container. Fails with kExists if the path is taken -- a
  // container is append-only and must never be silently re-created over.
  static Result<ContainerWriter> create(const std::string& path, uint32_t id,
                                        const std::array<uint8_t, 16>& store_uuid);

  // Reopen an existing container for appending after a crash: scans, truncates
  // any torn tail, and positions at the end of the last valid record.
  static Result<ContainerWriter> reopen(const std::string& path);

  // Appends one chunk. Returns the offset of its record header, which is what
  // goes into the index. kNoSpace when the container is full -- the caller rolls
  // to a new one rather than exceeding the 27-bit offset field.
  Result<uint32_t> append(const Digest32& fp, ByteSpan payload);

  bool fits(size_t payload_len) const {
    return size_ + ChunkHeader::kSize + payload_len <= kContainerMaxBytes;
  }
  uint64_t size() const { return size_; }
  uint32_t id() const { return id_; }
  const std::string& path() const { return file_.path(); }

  Status sync() { return file_.sync(); }
  Status close() { return file_.close(); }

 private:
  File file_;
  uint64_t size_ = 0;
  uint32_t id_ = 0;
  std::vector<uint8_t> scratch_;  // header+payload staging: one write() per chunk
};

class ContainerReader {
 public:
  ContainerReader() = default;
  // std::atomic is not movable, so these are spelled out rather than defaulted.
  ContainerReader(ContainerReader&& o) noexcept
      : file_(std::move(o.file_)), header_(o.header_),
        file_size_(o.file_size_.load(std::memory_order_relaxed)) {}
  ContainerReader& operator=(ContainerReader&& o) noexcept {
    if (this != &o) {
      file_ = std::move(o.file_);
      header_ = o.header_;
      file_size_.store(o.file_size_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
  }
  ContainerReader(const ContainerReader&) = delete;
  ContainerReader& operator=(const ContainerReader&) = delete;

  static Result<ContainerReader> open(const std::string& path);

  uint32_t id() const { return header_.container_id; }
  const ContainerHeader& header() const { return header_; }
  const std::string& path() const { return file_.path(); }

  // Reads one chunk. `length` comes from the index, so it is checked against the
  // record's own header; then the payload's CRC is verified, and if `expect_fp`
  // is given, its fingerprint too. Any mismatch is kCorrupt -- never a warning,
  // never a partial result (S1).
  Result<std::vector<uint8_t>> read(uint32_t offset, uint32_t length,
                                    const Digest32* expect_fp = nullptr);

  // Walk every record from the start. This is both the recovery path and the
  // index-rebuild path.
  Result<ScanResult> scan();

 private:
  File file_;
  ContainerHeader header_;
  // Atomic and refreshable: a reader may hold a container open while an ingest
  // thread is still appending to it, so a bounds check against a stale size
  // would wrongly report corruption for a record that genuinely exists.
  std::atomic<uint64_t> file_size_{0};
};

// Scan a container and truncate any torn tail, leaving a file that is entirely
// valid records. Returns the scan result from before the truncation.
Result<ScanResult> recover_container(const std::string& path);

}  // namespace dedupe
