#include "dedupe/container.h"

#include <fcntl.h>

#include "dedupe/crc32c.h"
#include "dedupe/sha256.h"

namespace dedupe {

// ---------------------------------------------------------------------------
// ContainerWriter
// ---------------------------------------------------------------------------

Result<ContainerWriter> ContainerWriter::create(const std::string& path, uint32_t id,
                                                const std::array<uint8_t, 16>& store_uuid) {
  // O_EXCL: creating over an existing container would destroy live chunks that
  // the index still points at. Fail loudly instead.
  ASSIGN_OR_RETURN(File f, File::open(path, O_WRONLY | O_CREAT | O_EXCL, 0644));

  ContainerHeader h;
  h.container_id = id;
  h.store_uuid = store_uuid;
  uint8_t buf[ContainerHeader::kSize];
  h.encode(buf);
  RETURN_IF_ERROR(f.write_all(ByteSpan(buf, sizeof(buf))));

  ContainerWriter w;
  w.file_ = std::move(f);
  w.size_ = ContainerHeader::kSize;
  w.flushed_ = ContainerHeader::kSize;
  w.id_ = id;
  return w;
}

Result<ContainerWriter> ContainerWriter::reopen(const std::string& path) {
  // Recovery first: whatever torn tail a crash left is removed before a single
  // new byte is appended. Appending onto a torn tail would bury the damage in
  // the middle of the file, where scan() could never resynchronize past it.
  ASSIGN_OR_RETURN(ScanResult scan, recover_container(path));

  ASSIGN_OR_RETURN(File f, File::open(path, O_WRONLY | O_APPEND));
  ContainerWriter w;
  w.file_ = std::move(f);
  w.size_ = scan.valid_bytes;
  w.flushed_ = scan.valid_bytes;
  w.id_ = scan.header.container_id;
  return w;
}

Result<uint32_t> ContainerWriter::append(const Digest32& fp, ByteSpan payload) {
  if (payload.empty()) return Status::invalid("append: empty payload");
  if (payload.size() > kMaxChunkLength) {
    return Status::invalid("append: payload " + std::to_string(payload.size()) +
                           " exceeds max chunk " + std::to_string(kMaxChunkLength));
  }
  if (!fits(payload.size())) {
    return Status::error(Err::kNoSpace, "container " + std::to_string(id_) + " full at " +
                                            std::to_string(size_) + " bytes");
  }

  ChunkHeader h;
  h.length = static_cast<uint32_t>(payload.size());
  h.crc32c = crc32c(payload);
  h.fp = fp;

  // Append the whole record to the write buffer. Header and payload always go
  // to the kernel together, so a crash cannot leave a header without its payload
  // in the same write -- and if it does tear, scan() detects it anyway.
  const uint64_t offset = size_;
  const size_t at = buf_.size();
  buf_.resize(at + ChunkHeader::kSize + payload.size());
  h.encode(buf_.data() + at);
  std::memcpy(buf_.data() + at + ChunkHeader::kSize, payload.data(), payload.size());
  size_ += ChunkHeader::kSize + payload.size();
  if (buf_.size() >= kWriteBuffer) RETURN_IF_ERROR(flush());
  // Guaranteed by fits(): the container never exceeds 128 MiB, so this cast is
  // safe. Asserted rather than assumed, because the whole index depends on it.
  if (offset >= kContainerMaxBytes) return Status::invalid("container offset overflow");
  return static_cast<uint32_t>(offset);
}

Status ContainerWriter::flush() {
  if (buf_.empty()) return Status();
  // Drop exactly the prefix that reached the file, even on failure. Keeping the
  // whole buffer after a partial write would re-send bytes the file already has,
  // duplicating a fragment of a record mid-container -- damage that scan() can
  // only respond to by truncating everything after it (CHALLENGES B12).
  size_t done = 0;
  const Status st = file_.write_all(buf_, &done);
  flushed_ += done;
  buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(done));
  RETURN_IF_ERROR(st);
  return Status();
}

Status ContainerWriter::sync() {
  RETURN_IF_ERROR(flush());
  return file_.sync();
}

Status ContainerWriter::close() {
  RETURN_IF_ERROR(flush());
  return file_.close();
}

// ---------------------------------------------------------------------------
// ContainerReader
// ---------------------------------------------------------------------------

Result<ContainerReader> ContainerReader::open(const std::string& path) {
  ASSIGN_OR_RETURN(File f, File::open(path, O_RDONLY));
  ASSIGN_OR_RETURN(const uint64_t sz, f.size());
  if (sz < ContainerHeader::kSize) {
    return Status::corrupt("container " + path + ": too small (" + std::to_string(sz) + " bytes)");
  }
  uint8_t buf[ContainerHeader::kSize];
  RETURN_IF_ERROR(f.pread_exact(buf, sizeof(buf), 0));
  ASSIGN_OR_RETURN(ContainerHeader h, ContainerHeader::decode(ByteSpan(buf, sizeof(buf))));

  ContainerReader r;
  r.file_ = std::move(f);
  r.header_ = h;
  r.file_size_.store(sz, std::memory_order_relaxed);
  return r;
}

Result<std::vector<uint8_t>> ContainerReader::read(uint32_t offset, uint32_t length,
                                                   const Digest32* expect_fp) {
  if (length == 0 || length > kMaxChunkLength) {
    return Status::invalid("read: bad length " + std::to_string(length));
  }
  const uint64_t need = static_cast<uint64_t>(offset) + ChunkHeader::kSize + length;
  // Bounds-check against the real file size before issuing the read, so a
  // corrupt index entry produces an error instead of a wild read (S9).
  if (need > file_size_.load(std::memory_order_relaxed)) {
    // The container may have grown since we opened it -- an ingest thread can be
    // appending while this reader is live. Refresh once before concluding the
    // index is wrong.
    ASSIGN_OR_RETURN(const uint64_t now, file_.size());
    file_size_.store(now, std::memory_order_relaxed);
  }
  if (need > file_size_.load(std::memory_order_relaxed)) {
    return Status::corrupt("read: record at " + std::to_string(offset) + "+" +
                           std::to_string(length) + " runs past end of " + file_.path() + " (" +
                           std::to_string(file_size_.load()) + " bytes)");
  }

  std::vector<uint8_t> buf(ChunkHeader::kSize + length);
  RETURN_IF_ERROR(file_.pread_exact(buf.data(), buf.size(), offset));
  ASSIGN_OR_RETURN(ChunkHeader h, ChunkHeader::decode(buf));

  if (h.length != length) {
    return Status::corrupt("read: header says " + std::to_string(h.length) +
                           " bytes but index says " + std::to_string(length));
  }
  const ByteSpan payload(buf.data() + ChunkHeader::kSize, length);
  const uint32_t got = crc32c(payload);
  if (got != h.crc32c) {
    return Status::corrupt("read: CRC mismatch at offset " + std::to_string(offset) + " in " +
                           file_.path());
  }
  if (expect_fp != nullptr && h.fp != *expect_fp) {
    // The index pointed at a real, intact chunk -- just not the one asked for.
    // That is an index bug or a truncated-key collision, and it must never be
    // resolved by returning the wrong bytes (S8).
    return Status::corrupt("read: fingerprint mismatch at offset " + std::to_string(offset));
  }

  buf.erase(buf.begin(), buf.begin() + ChunkHeader::kSize);
  return buf;
}

Status ContainerReader::refresh_size() {
  ASSIGN_OR_RETURN(const uint64_t now, file_.size());
  file_size_.store(now, std::memory_order_release);
  return Status();
}

Result<ScanResult> ContainerReader::scan() {
  // ALWAYS re-stat before scanning. This reader may have been cached since the
  // container was a 32-byte header, and scanning a stale size makes a full
  // container look empty (CHALLENGES B15).
  RETURN_IF_ERROR(refresh_size());
  ScanResult out;
  out.header = header_;
  out.valid_bytes = ContainerHeader::kSize;

  const uint64_t file_size = file_size_.load(std::memory_order_relaxed);
  uint64_t pos = ContainerHeader::kSize;
  std::vector<uint8_t> buf;
  while (pos < file_size) {
    uint8_t hdr[ChunkHeader::kSize];
    ASSIGN_OR_RETURN(const size_t got, file_.pread_upto(hdr, sizeof(hdr), pos));
    if (got < ChunkHeader::kSize) {
      out.torn = true;
      out.tear_reason = "partial header at offset " + std::to_string(pos) + " (" +
                        std::to_string(got) + " bytes)";
      break;
    }
    auto h = ChunkHeader::decode(ByteSpan(hdr, sizeof(hdr)));
    if (!h.ok()) {
      out.torn = true;
      out.tear_reason = "at offset " + std::to_string(pos) + ": " + h.status().message();
      break;
    }
    const uint32_t length = h.value().length;
    if (pos + ChunkHeader::kSize + length > file_size) {
      out.torn = true;
      out.tear_reason = "payload truncated at offset " + std::to_string(pos);
      break;
    }
    buf.resize(length);
    ASSIGN_OR_RETURN(const size_t pgot,
                     file_.pread_upto(buf.data(), length, pos + ChunkHeader::kSize));
    if (pgot < length) {
      out.torn = true;
      out.tear_reason = "short payload read at offset " + std::to_string(pos);
      break;
    }
    if (crc32c(buf) != h.value().crc32c) {
      out.torn = true;
      out.tear_reason = "CRC mismatch at offset " + std::to_string(pos);
      break;
    }
    out.records.push_back({h.value().fp, static_cast<uint32_t>(pos), length});
    pos += ChunkHeader::kSize + length;
    out.valid_bytes = pos;
  }
  return out;
}

Result<ScanResult> recover_container(const std::string& path) {
  ASSIGN_OR_RETURN(ContainerReader r, ContainerReader::open(path));
  ASSIGN_OR_RETURN(ScanResult scan, r.scan());
  if (scan.torn) {
    // Truncating is safe because the file is append-only: everything before
    // valid_bytes is a complete, CRC-verified record that nothing rewrites.
    ASSIGN_OR_RETURN(File f, File::open(path, O_WRONLY));
    RETURN_IF_ERROR(f.truncate(scan.valid_bytes));
    RETURN_IF_ERROR(f.sync());
    RETURN_IF_ERROR(f.close());
  }
  return scan;
}

}  // namespace dedupe
