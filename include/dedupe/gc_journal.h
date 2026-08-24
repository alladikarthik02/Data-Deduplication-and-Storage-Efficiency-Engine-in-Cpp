// The GC intent log.
//
// Compaction is the one operation in this engine that DESTROYS data: it copies
// live chunks to a new container and unlinks the old one. A crash in the middle
// must never leave both gone, and must never leave the index pointing at
// nothing. The journal is what makes each step recoverable:
//
//   BEGIN(src,dst)   we are about to copy src's live chunks into dst
//   COMMIT(src,dst)  dst is complete and fsynced; src is now redundant
//   DONE(src,dst)    src has been unlinked; nothing left to do
//   DELETE(src)      src had no live chunks at all; no copy needed
//
// Recovery reads the log and finishes or undoes whatever was in flight:
//   BEGIN with no COMMIT  -> dst is incomplete, src is untouched -> unlink dst
//   COMMIT with no DONE   -> dst is complete                     -> unlink src
// Both are idempotent, which is what lets recovery itself be crash-safe.
//
// The ordering rule that makes this work: COMMIT must be durable BEFORE src is
// unlinked. Everything else can be replayed.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "dedupe/io.h"
#include "dedupe/status.h"

namespace dedupe {

enum class GcOp : uint32_t {
  kBeginCompact = 1,
  kCommit = 2,
  kDone = 3,
  kDeleteContainer = 4,
};

struct GcJournalRecord {
  GcOp op = GcOp::kDone;
  uint32_t src = 0;
  uint32_t dst = 0;
  uint64_t seq = 0;
};

class GcJournal {
 public:
  static constexpr uint32_t kMagic = 0x524A4347;  // "GCJR"
  static constexpr size_t kRecordSize = 32;

  GcJournal() = default;
  // std::mutex is neither movable nor copyable, so the move operations are
  // spelled out: the destination gets a fresh mutex (nothing can be holding the
  // source's while it is being moved) and takes over the file and sequence.
  GcJournal(GcJournal&& o) noexcept : file_(std::move(o.file_)), seq_(o.seq_) {}
  GcJournal& operator=(GcJournal&& o) noexcept {
    if (this != &o) {
      file_ = std::move(o.file_);
      seq_ = o.seq_;
    }
    return *this;
  }
  GcJournal(const GcJournal&) = delete;
  GcJournal& operator=(const GcJournal&) = delete;

  static Result<GcJournal> open(const std::string& path);

  // Appends and fsyncs. Every journal record is a decision point, so none of
  // them is worth buffering.
  Status append(GcOp op, uint32_t src, uint32_t dst);
  Status reset();  // truncate: nothing is in flight

  // Reads whatever is there. A torn final record is dropped, exactly as with a
  // container: a partial record is an operation that never happened.
  static Result<std::vector<GcJournalRecord>> read_all(const std::string& path);

 private:
  File file_;
  std::mutex mu_;
  uint64_t seq_ = 0;
};

}  // namespace dedupe
