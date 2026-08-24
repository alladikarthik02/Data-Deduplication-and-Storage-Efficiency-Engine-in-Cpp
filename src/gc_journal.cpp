#include "dedupe/gc_journal.h"

#include <fcntl.h>

#include "dedupe/crc32c.h"
#include "dedupe/format.h"

namespace dedupe {

Result<GcJournal> GcJournal::open(const std::string& path) {
  ASSIGN_OR_RETURN(File f, File::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644));
  GcJournal j;
  j.file_ = std::move(f);
  ASSIGN_OR_RETURN(const uint64_t sz, j.file_.size());
  j.seq_ = sz / kRecordSize;
  return j;
}

Status GcJournal::append(GcOp op, uint32_t src, uint32_t dst) {
  std::lock_guard<std::mutex> lk(mu_);
  uint8_t rec[kRecordSize] = {};
  put_u32le(rec + 0, kMagic);
  put_u32le(rec + 4, static_cast<uint32_t>(op));
  put_u32le(rec + 8, src);
  put_u32le(rec + 12, dst);
  put_u64le(rec + 16, seq_++);
  put_u32le(rec + 24, crc32c(ByteSpan(rec, 24)));
  RETURN_IF_ERROR(file_.write_all(ByteSpan(rec, kRecordSize)));
  // Durable before the action it describes. A journal entry that is still in the
  // page cache when the process dies is an entry that never existed.
  return file_.sync();
}

Status GcJournal::reset() {
  std::lock_guard<std::mutex> lk(mu_);
  RETURN_IF_ERROR(file_.truncate(0));
  RETURN_IF_ERROR(file_.sync());
  seq_ = 0;
  return Status();
}

Result<std::vector<GcJournalRecord>> GcJournal::read_all(const std::string& path) {
  std::vector<GcJournalRecord> out;
  auto exists = path_exists(path);
  if (!exists.ok()) return exists.status();
  if (!exists.value()) return out;

  ASSIGN_OR_RETURN(std::vector<uint8_t> raw, read_file(path));
  for (size_t off = 0; off + kRecordSize <= raw.size(); off += kRecordSize) {
    const uint8_t* p = raw.data() + off;
    if (get_u32le(p) != kMagic) break;
    if (crc32c(ByteSpan(p, 24)) != get_u32le(p + 24)) break;
    GcJournalRecord r;
    r.op = static_cast<GcOp>(get_u32le(p + 4));
    r.src = get_u32le(p + 8);
    r.dst = get_u32le(p + 12);
    r.seq = get_u64le(p + 16);
    out.push_back(r);
  }
  // A trailing partial record is silently ignored: the operation it would have
  // described never became durable, so it never happened.
  return out;
}

}  // namespace dedupe
