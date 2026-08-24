#include "dedupe/store.h"

#include <fcntl.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <random>

#include "dedupe/crc32c.h"
#include "dedupe/gc_journal.h"
#include "dedupe/sha256.h"

namespace dedupe {
namespace {

constexpr uint32_t kSuperMagic = 0x50555444;  // "DTUP"
constexpr uint32_t kSuperVersion = 1;
constexpr size_t kSuperSize = 64;
// Sync the active container roughly every 8 MiB of appends so a long ingest is
// not one enormous unflushed extent. This is a memory/latency knob, not a
// correctness one: correctness comes from the fsync before the recipe publish.
constexpr uint64_t kSyncEvery = 8u << 20;

std::string hex16(const std::array<uint8_t, 16>& u) {
  return to_hex(ByteSpan(u.data(), u.size()));
}

}  // namespace

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

std::string Store::container_path(uint32_t id) const {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "c%08u.dat", id);
  return path_join(containers_dir(), buf);
}

// Recipes are named by the hash of the file name, never by the name itself:
// the filesystem never sees user-controlled bytes (S12).
std::string Store::recipe_path(const std::string& name) const {
  const Digest32 h = sha256(as_bytes(name));
  return path_join(recipes_dir(), to_hex(h) + ".rcp");
}

// ---------------------------------------------------------------------------
// open / superblock / recovery
// ---------------------------------------------------------------------------

Status Store::write_superblock() {
  std::vector<uint8_t> buf(kSuperSize, 0);
  put_u32le(buf.data() + 0, kSuperMagic);
  put_u32le(buf.data() + 4, kSuperVersion);
  std::memcpy(buf.data() + 8, uuid_.data(), 16);
  put_u32le(buf.data() + 24, opts_.chunker.min_size);
  put_u32le(buf.data() + 28, opts_.chunker.avg_size);
  put_u32le(buf.data() + 32, opts_.chunker.max_size);
  put_u64le(buf.data() + 36, opts_.chunker.mask_s);
  put_u64le(buf.data() + 44, opts_.chunker.mask_l);
  put_u32le(buf.data() + 52, crc32c(ByteSpan(buf.data(), 52)));
  return write_file_atomic(path_join(path_, "STORE"), buf);
}

Status Store::read_superblock() {
  ASSIGN_OR_RETURN(std::vector<uint8_t> buf, read_file(path_join(path_, "STORE")));
  if (buf.size() < kSuperSize) return Status::corrupt("STORE: truncated superblock");
  if (get_u32le(buf.data()) != kSuperMagic) return Status::corrupt("STORE: bad magic");
  if (get_u32le(buf.data() + 4) != kSuperVersion) {
    return Status::corrupt("STORE: unsupported version " +
                           std::to_string(get_u32le(buf.data() + 4)));
  }
  if (crc32c(ByteSpan(buf.data(), 52)) != get_u32le(buf.data() + 52)) {
    return Status::corrupt("STORE: superblock CRC mismatch");
  }
  std::memcpy(uuid_.data(), buf.data() + 8, 16);
  // The chunker parameters are STORE state, not caller state: re-chunking an
  // existing store with different parameters would produce different boundaries
  // and silently stop deduplicating against everything already in it.
  opts_.chunker.min_size = get_u32le(buf.data() + 24);
  opts_.chunker.avg_size = get_u32le(buf.data() + 28);
  opts_.chunker.max_size = get_u32le(buf.data() + 32);
  opts_.chunker.mask_s = get_u64le(buf.data() + 36);
  opts_.chunker.mask_l = get_u64le(buf.data() + 44);
  if (!opts_.chunker.valid()) return Status::corrupt("STORE: invalid chunker parameters");
  return Status();
}

// The index is rebuilt from the containers on every open. That is not a
// fallback path -- it is the normal one, and it is what makes index durability a
// performance concern rather than a correctness one (SPEC 3.0).
Status Store::rebuild_index() {
  ASSIGN_OR_RETURN(std::vector<std::string> entries, list_dir(containers_dir()));
  uint32_t max_id = 0;
  uint64_t physical = 0;
  for (const std::string& name : entries) {
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".dat") != 0) continue;
    const std::string path = path_join(containers_dir(), name);

    // Recover first: a torn tail from a previous crash is truncated before its
    // records are indexed, so the index never points into damaged bytes (S3).
    ASSIGN_OR_RETURN(ScanResult scan, recover_container(path));
    if (scan.header.store_uuid != uuid_) {
      return Status::corrupt("container " + name + " belongs to store " +
                             hex16(scan.header.store_uuid) + ", not " + hex16(uuid_));
    }
    for (const ScanRecord& rec : scan.records) {
      Location loc;
      loc.container_id = scan.header.container_id;
      loc.offset = rec.offset;
      loc.length = rec.length;
      auto ins = index_.insert(rec.fp, loc);
      RETURN_IF_ERROR(ins.status());
      // A duplicate here means the same chunk was written twice -- possible when
      // two ingest threads raced (SPEC 3.5) or a crash re-ran work. The first
      // copy wins; the second is garbage for GC. Not an error.
    }
    max_id = std::max(max_id, scan.header.container_id);
    physical += scan.valid_bytes;
  }
  next_container_id_.store(entries.empty() ? 0 : max_id + 1, std::memory_order_relaxed);
  physical_bytes_.store(physical, std::memory_order_relaxed);
  return Status();
}

Result<std::unique_ptr<Store>> Store::open(const std::string& path, StoreOptions opts) {
  if (!opts.chunker.valid()) return Status::invalid("store: invalid chunker parameters");

  auto store = std::unique_ptr<Store>(new Store());
  store->path_ = path;
  store->opts_ = opts;

  RETURN_IF_ERROR(make_dirs(path));
  RETURN_IF_ERROR(make_dirs(store->containers_dir()));
  RETURN_IF_ERROR(make_dirs(store->recipes_dir()));

  // S14: exclusive for the lifetime of this object. Taken before any state is
  // read, so two processes can never both be mid-recovery on the same store.
  ASSIGN_OR_RETURN(store->lock_, FileLock::acquire(path_join(path, "LOCK")));

  ASSIGN_OR_RETURN(const bool have_super, path_exists(path_join(path, "STORE")));
  if (have_super) {
    RETURN_IF_ERROR(store->read_superblock());
  } else {
    std::random_device rd;
    for (size_t i = 0; i < 16; i++) store->uuid_[i] = static_cast<uint8_t>(rd() & 0xFF);
    RETURN_IF_ERROR(store->write_superblock());
  }

  // BEFORE the index is built: finish or undo whatever a crash interrupted, so
  // the index is never built over a container that is about to be unlinked.
  RETURN_IF_ERROR(store->replay_gc_journal());
  ASSIGN_OR_RETURN(store->journal_, GcJournal::open(store->gc_journal_path()));

  RETURN_IF_ERROR(store->rebuild_index());
  return store;
}

Store::~Store() {
  // Any session that outlived its Store, or was abandoned without finish(),
  // leaves an open slot. Flush and close them so the container files on disk are
  // complete rather than missing their buffered tail.
  std::unique_lock<std::shared_mutex> lk(slots_mu_);
  for (auto& [id, slot] : slots_) {
    std::lock_guard<std::mutex> slk(slot->mu);
    if (!slot->open.load(std::memory_order_relaxed)) continue;
    (void)slot->writer.sync();
    (void)slot->writer.close();
    slot->open.store(false, std::memory_order_relaxed);
  }
}

// Replay the GC intent log (SPEC 3.6). Both branches are idempotent, so a crash
// during recovery is just another crash to recover from.
Status Store::replay_gc_journal() {
  ASSIGN_OR_RETURN(std::vector<GcJournalRecord> records,
                   GcJournal::read_all(gc_journal_path()));
  if (records.empty()) return Status();

  struct Pending {
    uint32_t src = 0;
    bool committed = false;
    bool done = false;
    bool delete_only = false;
  };
  std::map<uint32_t, Pending> by_dst;  // keyed by dst, or by src for DELETE
  for (const GcJournalRecord& r : records) {
    switch (r.op) {
      case GcOp::kBeginCompact: by_dst[r.dst] = Pending{r.src, false, false, false}; break;
      case GcOp::kCommit: by_dst[r.dst].committed = true; by_dst[r.dst].src = r.src; break;
      case GcOp::kDone: by_dst[r.dst].done = true; break;
      case GcOp::kDeleteContainer:
        by_dst[r.src] = Pending{r.src, true, false, true};
        break;
    }
  }

  size_t undone = 0, finished = 0;
  for (const auto& [dst, p] : by_dst) {
    if (p.done) continue;
    if (p.delete_only) {
      // The container had no live chunks. Removing it again is harmless.
      const Status st = unlink_file(container_path(p.src));
      if (!st.ok() && st.code() != Err::kNotFound) return st;
      finished++;
    } else if (p.committed) {
      // dst is complete and durable, so src is redundant -- finish the job.
      const Status st = unlink_file(container_path(p.src));
      if (!st.ok() && st.code() != Err::kNotFound) return st;
      finished++;
    } else {
      // We never committed, so dst is an incomplete copy and src is untouched.
      // Throw the copy away.
      const Status st = unlink_file(container_path(dst));
      if (!st.ok() && st.code() != Err::kNotFound) return st;
      undone++;
    }
  }
  if (undone > 0 || finished > 0) {
    std::fprintf(stderr, "dedupe: gc recovery -- %zu compaction(s) finished, %zu undone\n",
                 finished, undone);
  }
  // Nothing is in flight any more.
  ASSIGN_OR_RETURN(GcJournal j, GcJournal::open(gc_journal_path()));
  return j.reset();
}

uint32_t Store::register_ingest() {
  std::lock_guard<std::mutex> lk(active_mu_);
  const uint32_t e = index_.epoch();
  active_ingests_.insert(e);
  return e;
}

void Store::unregister_ingest(uint32_t epoch) {
  std::lock_guard<std::mutex> lk(active_mu_);
  auto it = active_ingests_.find(epoch);
  if (it != active_ingests_.end()) active_ingests_.erase(it);
}

uint32_t Store::fence_and_safe_epoch(uint32_t* out_fence) {
  std::lock_guard<std::mutex> lk(active_mu_);
  const uint32_t fence = index_.bump_epoch();
  if (out_fence != nullptr) *out_fence = fence;
  // Nothing in flight: the fence itself is the watermark. Otherwise hold back to
  // the oldest ingest still running -- anything it looked up before the fence is
  // still fair game for a recipe it has not published yet.
  if (active_ingests_.empty()) return fence;
  return std::min(fence, *active_ingests_.begin());
}

Result<std::vector<uint32_t>> Store::container_ids() {
  ASSIGN_OR_RETURN(std::vector<std::string> names, list_dir(containers_dir()));
  std::vector<uint32_t> ids;
  for (const std::string& n : names) {
    if (n.size() != 13 || n[0] != 'c' || n.compare(9, 4, ".dat") != 0) continue;
    ids.push_back(static_cast<uint32_t>(std::strtoul(n.substr(1, 8).c_str(), nullptr, 10)));
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

bool Store::container_is_open(uint32_t id) {
  auto slot = find_slot(id);
  return slot != nullptr && slot->open.load(std::memory_order_acquire);
}

Result<ContainerWriter> Store::create_container_for_gc() {
  const uint32_t id = next_container_id_.fetch_add(1, std::memory_order_relaxed);
  if (id > Location::kMaxContainer) {
    return Status::error(Err::kNoSpace, "store is full: container id overflow");
  }
  return ContainerWriter::create(container_path(id), id, uuid_);
}

Status Store::retire_container(uint32_t id) {
  {
    std::unique_lock<std::shared_mutex> lk(readers_mu_);
    readers_.erase(id);  // any shared_ptr already handed out keeps its fd alive
  }
  {
    std::unique_lock<std::shared_mutex> lk(slots_mu_);
    slots_.erase(id);
  }
  const Status st = unlink_file(container_path(id));
  if (!st.ok() && st.code() != Err::kNotFound) return st;
  return Status();
}

Result<std::shared_ptr<ContainerSlot>> Store::new_container() {
  // fetch_add so two ingest threads never pick the same id.
  const uint32_t id = next_container_id_.fetch_add(1, std::memory_order_relaxed);
  if (id > Location::kMaxContainer) {
    return Status::error(Err::kNoSpace, "store is full: container id " + std::to_string(id) +
                                            " exceeds the 16-bit field");
  }
  ASSIGN_OR_RETURN(ContainerWriter w, ContainerWriter::create(container_path(id), id, uuid_));
  auto slot = std::make_shared<ContainerSlot>();
  slot->visible.store(w.flushed(), std::memory_order_relaxed);
  slot->durable.store(0, std::memory_order_relaxed);
  slot->writer = std::move(w);
  {
    std::unique_lock<std::shared_mutex> lk(slots_mu_);
    slots_[id] = slot;
  }
  return slot;
}

std::shared_ptr<ContainerSlot> Store::find_slot(uint32_t id) {
  std::shared_lock<std::shared_mutex> lk(slots_mu_);
  auto it = slots_.find(id);
  return (it == slots_.end()) ? nullptr : it->second;
}

Status Store::ensure_visible(uint32_t id, uint64_t end) {
  auto slot = find_slot(id);
  // No slot means the container was closed (which flushes) or predates this
  // process, so everything on disk is already visible.
  if (slot == nullptr) return Status();
  if (slot->visible.load(std::memory_order_acquire) >= end) return Status();
  std::lock_guard<std::mutex> lk(slot->mu);
  if (slot->visible.load(std::memory_order_relaxed) >= end) return Status();
  if (!slot->open.load(std::memory_order_relaxed)) return Status();
  RETURN_IF_ERROR(slot->writer.flush());
  slot->visible.store(slot->writer.flushed(), std::memory_order_release);
  return Status();
}

Status Store::ensure_durable(uint32_t id, uint64_t end) {
  auto slot = find_slot(id);
  if (slot == nullptr) return Status();
  if (slot->durable.load(std::memory_order_acquire) >= end) return Status();
  std::lock_guard<std::mutex> lk(slot->mu);
  if (slot->durable.load(std::memory_order_relaxed) >= end) return Status();
  if (!slot->open.load(std::memory_order_relaxed)) return Status();
  forced_syncs_.fetch_add(1, std::memory_order_relaxed);
  RETURN_IF_ERROR(slot->writer.sync());
  const uint64_t now = slot->writer.size();
  slot->visible.store(now, std::memory_order_release);
  slot->durable.store(now, std::memory_order_release);
  return Status();
}

Result<std::shared_ptr<ContainerReader>> Store::reader_for(uint32_t id) {
  {
    std::shared_lock<std::shared_mutex> lk(readers_mu_);
    auto it = readers_.find(id);
    if (it != readers_.end()) return it->second;
  }
  std::unique_lock<std::shared_mutex> lk(readers_mu_);
  auto it = readers_.find(id);
  if (it != readers_.end()) return it->second;  // another thread won the race
  ASSIGN_OR_RETURN(ContainerReader r, ContainerReader::open(container_path(id)));
  auto sp = std::make_shared<ContainerReader>(std::move(r));
  readers_[id] = sp;
  return sp;
}

// ---------------------------------------------------------------------------
// IngestSession
// ---------------------------------------------------------------------------

IngestSession::~IngestSession() { (void)finish(); }

IngestSession::IngestSession(IngestSession&& o) noexcept
    : store_(o.store_), slot_(std::move(o.slot_)), bytes_since_sync_(o.bytes_since_sync_) {
  o.store_ = nullptr;
}
IngestSession& IngestSession::operator=(IngestSession&& o) noexcept {
  if (this != &o) {
    (void)finish();
    store_ = o.store_;
    slot_ = std::move(o.slot_);
    bytes_since_sync_ = o.bytes_since_sync_;
    o.store_ = nullptr;
  }
  return *this;
}

Status IngestSession::finish() {
  if (slot_ == nullptr) return Status();
  auto slot = slot_;
  slot_.reset();
  std::lock_guard<std::mutex> lk(slot->mu);
  if (!slot->open.load(std::memory_order_relaxed)) return Status();
  RETURN_IF_ERROR(slot->writer.sync());
  const uint64_t n = slot->writer.size();
  slot->visible.store(n, std::memory_order_release);
  slot->durable.store(n, std::memory_order_release);
  RETURN_IF_ERROR(slot->writer.close());
  slot->open.store(false, std::memory_order_release);
  return Status();
}

void IngestSession::note_reference(std::map<uint32_t, uint64_t>* referenced,
                                   const Location& loc) {
  const uint64_t end = static_cast<uint64_t>(loc.offset) + ChunkHeader::kSize + loc.length;
  auto it = referenced->find(loc.container_id);
  if (it == referenced->end() || it->second < end) (*referenced)[loc.container_id] = end;
}

// S4 across threads. A recipe may reference chunks written by OTHER sessions
// whose containers we do not own, so durability has to be requested per
// container rather than assumed for our own (CHALLENGES B11).
Status IngestSession::make_referenced_durable(const std::map<uint32_t, uint64_t>& referenced) {
  for (const auto& [cid, end] : referenced) {
    RETURN_IF_ERROR(store_->ensure_durable(cid, end));
  }
  return Status();
}

Result<Location> IngestSession::write_chunk(const Digest32& fp, ByteSpan payload) {
  if (slot_ == nullptr || !slot_->writer.fits(payload.size())) {
    if (slot_ != nullptr) RETURN_IF_ERROR(finish());  // roll over: sync, close, release
    ASSIGN_OR_RETURN(slot_, store_->new_container());
    bytes_since_sync_ = 0;
  }
  uint32_t offset = 0;
  {
    // Per-container lock. Each session owns its own container, so this is
    // uncontended in the common case; it exists so that another session's
    // ensure_durable() can safely reach in and fsync.
    std::lock_guard<std::mutex> lk(slot_->mu);
    ASSIGN_OR_RETURN(offset, slot_->writer.append(fp, payload));
    slot_->visible.store(slot_->writer.flushed(), std::memory_order_release);
  }
  bytes_since_sync_ += ChunkHeader::kSize + payload.size();
  if (bytes_since_sync_ >= kSyncEvery) {
    RETURN_IF_ERROR(store_->ensure_durable(slot_->writer.id(), slot_->writer.size()));
    bytes_since_sync_ = 0;
  }

  Location loc;
  loc.container_id = slot_->writer.id();
  loc.offset = offset;
  loc.length = static_cast<uint32_t>(payload.size());
  return loc;
}

namespace {
// RAII: a leaked registration would pin GC's reclaim watermark forever, so this
// must survive every early return and error path in the ingest routines.
struct IngestRegistration {
  Store* store;
  uint32_t epoch;
  explicit IngestRegistration(Store* s) : store(s), epoch(s->register_ingest()) {}
  ~IngestRegistration() { store->unregister_ingest(epoch); }
  IngestRegistration(const IngestRegistration&) = delete;
  IngestRegistration& operator=(const IngestRegistration&) = delete;
};
}  // namespace

Result<IngestStats> IngestSession::ingest_bytes(const std::string& name, ByteSpan data) {
  if (name.empty()) return Status::invalid("ingest: empty name");
  if (store_ == nullptr) return Status::invalid("ingest: session is finished");
  IngestRegistration reg(store_);

  IngestStats stats;
  Recipe recipe;
  recipe.name = name;

  Sha256 whole;
  Chunker chunker(store_->opts_.chunker);
  Status first_error;

  std::map<uint32_t, uint64_t> referenced;

  auto handle = [&](ByteSpan chunk) {
    if (!first_error.ok()) return;
    const Digest32 fp = sha256(chunk);
    stats.chunks_total++;

    Location loc;
    if (store_->index_.lookup(fp, &loc)) {
      stats.chunks_deduped++;
      note_reference(&referenced, loc);  // someone else's container may need syncing
    } else {
      auto written = write_chunk(fp, chunk);
      if (!written.ok()) {
        first_error = written.status();
        return;
      }
      auto ins = store_->index_.insert(fp, written.value());
      if (!ins.ok()) {
        first_error = ins.status();
        return;
      }
      note_reference(&referenced, ins.value().loc);
      if (ins.value().inserted) {
        stats.chunks_new++;
        stats.physical_bytes += ChunkHeader::kSize + chunk.size();
      } else {
        // Lost an insert race with another ingest thread. We keep THEIR
        // location: recipes written moments ago may already reference it, and
        // repointing the index would strand them. Our copy simply becomes
        // garbage for GC to reclaim -- the cost of not taking a global lock on
        // the write path (SPEC 3.5).
        stats.chunks_deduped++;
        stats.physical_bytes += ChunkHeader::kSize + chunk.size();
      }
    }
    RecipeEntry e;
    e.fp = fp;
    e.length = static_cast<uint32_t>(chunk.size());
    recipe.entries.push_back(e);
    stats.logical_bytes += chunk.size();
  };

  whole.update(data);
  chunker.feed(data, handle);
  chunker.finish(handle);
  RETURN_IF_ERROR(first_error);

  recipe.logical_size = stats.logical_bytes;
  recipe.file_sha256 = whole.finish();
  if (recipe.logical_size != data.size()) {
    return Status::error(Err::kInvalid, "ingest: chunker lost bytes (" +
                                            std::to_string(recipe.logical_size) + " vs " +
                                            std::to_string(data.size()) + ")");
  }

  // S4 -- ORDER. The chunks must be durable before the recipe that references
  // them exists, or a crash leaves a file that claims to exist and cannot be
  // read. write_file_atomic then fsyncs the recipe and its directory.
  // S4 -- ORDER, and across every container involved, not just our own.
  RETURN_IF_ERROR(make_referenced_durable(referenced));
  RETURN_IF_ERROR(write_file_atomic(store_->recipe_path(name), recipe.encode()));
  return stats;
}

Result<IngestStats> IngestSession::ingest_file(const std::string& name,
                                               const std::string& source_path) {
  if (store_ == nullptr) return Status::invalid("ingest: session is finished");
  IngestRegistration reg(store_);
  // S12: opened read-only. Ingest never modifies the user's source data.
  ASSIGN_OR_RETURN(File in, File::open(source_path, O_RDONLY));

  IngestStats stats;
  Recipe recipe;
  recipe.name = name;
  Sha256 whole;
  Chunker chunker(store_->opts_.chunker);
  Status first_error;
  std::map<uint32_t, uint64_t> referenced;

  auto handle = [&](ByteSpan chunk) {
    if (!first_error.ok()) return;
    const Digest32 fp = sha256(chunk);
    stats.chunks_total++;
    Location loc;
    if (store_->index_.lookup(fp, &loc)) {
      stats.chunks_deduped++;
      note_reference(&referenced, loc);
    } else {
      auto written = write_chunk(fp, chunk);
      if (!written.ok()) {
        first_error = written.status();
        return;
      }
      auto ins = store_->index_.insert(fp, written.value());
      if (!ins.ok()) {
        first_error = ins.status();
        return;
      }
      note_reference(&referenced, ins.value().loc);
      if (ins.value().inserted) stats.chunks_new++;
      else stats.chunks_deduped++;
      stats.physical_bytes += ChunkHeader::kSize + chunk.size();
    }
    RecipeEntry e;
    e.fp = fp;
    e.length = static_cast<uint32_t>(chunk.size());
    recipe.entries.push_back(e);
    stats.logical_bytes += chunk.size();
  };

  // Streaming, never whole-file: S10 requires ingest memory to be bounded
  // regardless of input size.
  std::vector<uint8_t> buf(store_->opts_.read_buffer_bytes);
  uint64_t offset = 0;
  while (true) {
    ASSIGN_OR_RETURN(const size_t got, in.pread_upto(buf.data(), buf.size(), offset));
    if (got == 0) break;
    const ByteSpan span(buf.data(), got);
    whole.update(span);
    chunker.feed(span, handle);
    RETURN_IF_ERROR(first_error);
    offset += got;
  }
  chunker.finish(handle);
  RETURN_IF_ERROR(first_error);

  recipe.logical_size = stats.logical_bytes;
  recipe.file_sha256 = whole.finish();
  if (recipe.logical_size != offset) {
    return Status::error(Err::kInvalid, "ingest: chunker lost bytes");
  }

  RETURN_IF_ERROR(make_referenced_durable(referenced));
  RETURN_IF_ERROR(write_file_atomic(store_->recipe_path(name), recipe.encode()));
  return stats;
}

// ---------------------------------------------------------------------------
// Store operations
// ---------------------------------------------------------------------------

Result<IngestSession> Store::begin_ingest() {
  IngestSession s;
  s.store_ = this;
  return s;
}

Result<IngestStats> Store::ingest_bytes(const std::string& name, ByteSpan data) {
  ASSIGN_OR_RETURN(IngestSession s, begin_ingest());
  auto r = s.ingest_bytes(name, data);
  RETURN_IF_ERROR(s.finish());
  return r;
}

Result<IngestStats> Store::ingest_file(const std::string& name, const std::string& src) {
  ASSIGN_OR_RETURN(IngestSession s, begin_ingest());
  auto r = s.ingest_file(name, src);
  RETURN_IF_ERROR(s.finish());
  return r;
}

Result<std::vector<uint8_t>> Store::read_chunk(const Digest32& fp) {
  // Two attempts, because GC's copy-forward compaction can move a chunk between
  // our index lookup and our read: we would then hold a location whose container
  // has just been unlinked. Re-consulting the index gives the new location. The
  // window is small but it is real, and the alternative -- pinning every
  // container a reader might ever consult -- would defeat compaction entirely.
  Status last;
  for (int attempt = 0; attempt < 2; attempt++) {
    Location loc;
    if (!index_.lookup(fp, &loc)) {
      return Status::error(Err::kNotFound, "chunk " + to_hex(fp).substr(0, 16) + " not in index");
    }
    // The record may still be in an open writer's buffer, invisible to a reader.
    // Flushing is enough here -- durability is the publish path's problem.
    const uint64_t end = static_cast<uint64_t>(loc.offset) + ChunkHeader::kSize + loc.length;
    RETURN_IF_ERROR(ensure_visible(loc.container_id, end));
    auto reader = reader_for(loc.container_id);
    if (reader.ok()) {
      auto data = reader.value()->read(loc.offset, loc.length,
                                       opts_.verify_fingerprint_on_read ? &fp : nullptr);
      if (data.ok()) return data;
      last = data.status();
    } else {
      last = reader.status();
#ifdef DEDUPE_DEBUG_READ
      std::fprintf(stderr,
                   "[read_chunk] fp=%s -> container=%u offset=%u length=%u : %s\n",
                   to_hex(fp).substr(0, 16).c_str(), loc.container_id, loc.offset, loc.length,
                   reader.status().message().c_str());
#endif
    }
  }
  return last;
}

Result<Recipe> Store::read_recipe(const std::string& name) {
  ASSIGN_OR_RETURN(std::vector<uint8_t> raw, read_file(recipe_path(name)));
  ASSIGN_OR_RETURN(Recipe r, Recipe::decode(raw));
  // The recipe carries the name it was written with. If it disagrees with the
  // one we hashed to find it, something is very wrong -- a hash collision or a
  // hand-edited store -- and returning the wrong file is not an option.
  if (r.name != name) {
    return Status::corrupt("recipe for '" + name + "' contains name '" + r.name + "'");
  }
  return r;
}

Result<bool> Store::exists(const std::string& name) { return path_exists(recipe_path(name)); }

Result<std::vector<uint8_t>> Store::restore_bytes(const std::string& name) {
  ASSIGN_OR_RETURN(Recipe r, read_recipe(name));
  std::vector<uint8_t> out;
  out.reserve(static_cast<size_t>(r.logical_size));
  Sha256 whole;
  for (const RecipeEntry& e : r.entries) {
    ASSIGN_OR_RETURN(std::vector<uint8_t> chunk, read_chunk(e.fp));
    if (chunk.size() != e.length) {
      return Status::corrupt("restore: chunk length " + std::to_string(chunk.size()) +
                             " != recipe's " + std::to_string(e.length));
    }
    whole.update(chunk);
    out.insert(out.end(), chunk.begin(), chunk.end());
  }
  // S1 -- the end-to-end check. Per-chunk CRCs and fingerprints can all pass
  // while the recipe itself lists the wrong chunks in the wrong order; only the
  // whole-file digest catches that.
  if (whole.finish() != r.file_sha256) {
    return Status::corrupt("restore: whole-file digest mismatch for '" + name + "'");
  }
  return out;
}

Status Store::restore(const std::string& name, const std::string& out_path) {
  ASSIGN_OR_RETURN(Recipe r, read_recipe(name));

  // Write to a temporary and rename: a failed restore must never leave a
  // partial file sitting at the destination looking complete.
  const std::string tmp = out_path + ".restore-tmp";
  {
    ASSIGN_OR_RETURN(File out, File::open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644));
    Sha256 whole;
    for (const RecipeEntry& e : r.entries) {
      ASSIGN_OR_RETURN(std::vector<uint8_t> chunk, read_chunk(e.fp));
      if (chunk.size() != e.length) {
        (void)unlink_file(tmp);
        return Status::corrupt("restore: chunk length mismatch");
      }
      whole.update(chunk);
      RETURN_IF_ERROR(out.write_all(chunk));
    }
    if (whole.finish() != r.file_sha256) {
      (void)unlink_file(tmp);
      return Status::corrupt("restore: whole-file digest mismatch for '" + name + "'");
    }
    RETURN_IF_ERROR(out.sync());
    RETURN_IF_ERROR(out.close());
  }
  RETURN_IF_ERROR(rename_file(tmp, out_path));
  return Status();
}

Status Store::remove(const std::string& name) {
  ASSIGN_OR_RETURN(const bool have, path_exists(recipe_path(name)));
  if (!have) return Status::error(Err::kNotFound, "no such file: " + name);
  // Only the recipe goes. Chunks are freed by GC, never inline: another file may
  // reference the same chunk, and deciding that inline would need a global scan
  // on the delete path (SPEC 3.6).
  return unlink_file(recipe_path(name));
}

Result<std::vector<Recipe>> Store::all_recipes() {
  ASSIGN_OR_RETURN(std::vector<std::string> names, list_dir(recipes_dir()));
  std::vector<Recipe> out;
  for (const std::string& f : names) {
    if (f.size() < 4 || f.compare(f.size() - 4, 4, ".rcp") != 0) continue;
    auto raw = read_file(path_join(recipes_dir(), f));
    if (!raw.ok()) {
      // A concurrent remove() between list_dir and read_file is an ordinary
      // race, not an error: the file simply is not there any more. Any other
      // failure still propagates.
      if (raw.status().code() == Err::kNotFound) continue;
      return raw.status();
    }
    ASSIGN_OR_RETURN(Recipe r, Recipe::decode(raw.take()));
    out.push_back(std::move(r));
  }
  return out;
}

Result<std::vector<FileInfo>> Store::list() {
  ASSIGN_OR_RETURN(std::vector<Recipe> recipes, all_recipes());
  std::vector<FileInfo> out;
  out.reserve(recipes.size());
  for (const Recipe& r : recipes) {
    out.push_back({r.name, r.logical_size, static_cast<uint64_t>(r.entries.size())});
  }
  std::sort(out.begin(), out.end(),
            [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
  return out;
}

Result<StoreStats> Store::stats() {
  StoreStats st;
  ASSIGN_OR_RETURN(std::vector<Recipe> recipes, all_recipes());
  for (const Recipe& r : recipes) {
    st.files++;
    st.logical_bytes += r.logical_size;
    st.chunk_references += r.entries.size();
  }
  ASSIGN_OR_RETURN(std::vector<std::string> cnames, list_dir(containers_dir()));
  for (const std::string& n : cnames) {
    if (n.size() < 4 || n.compare(n.size() - 4, 4, ".dat") != 0) continue;
    ASSIGN_OR_RETURN(const uint64_t sz, file_size(path_join(containers_dir(), n)));
    st.physical_bytes += sz;
    st.containers++;
  }
  st.unique_chunks = index_.size();
  st.index_bytes = index_.memory_bytes();
  return st;
}

Result<VerifyReport> Store::verify(VerifyLevel level) {
  VerifyReport rep;
  auto note = [&](const std::string& s) {
    if (rep.problems.size() < 100) rep.problems.push_back(s);
  };

  ASSIGN_OR_RETURN(std::vector<Recipe> recipes, all_recipes());
  for (const Recipe& r : recipes) {
    rep.files_checked++;
    uint64_t sum = 0;
    Sha256 whole;
    for (const RecipeEntry& e : r.entries) {
      rep.chunks_checked++;
      sum += e.length;
      if (level == VerifyLevel::kQuick) {
        Location loc;
        if (!index_.lookup(e.fp, &loc)) {
          note("file '" + r.name + "': chunk " + to_hex(e.fp).substr(0, 16) + " missing");
          continue;
        }
        if (loc.length != e.length) {
          note("file '" + r.name + "': chunk " + to_hex(e.fp).substr(0, 16) + " length " +
               std::to_string(loc.length) + " != recipe's " + std::to_string(e.length));
        }
        continue;
      }
      // Deep: actually read every chunk back, verifying CRC and fingerprint.
      auto chunk = read_chunk(e.fp);
      if (!chunk.ok()) {
        note("file '" + r.name + "': " + chunk.status().to_string());
        continue;
      }
      if (chunk.value().size() != e.length) {
        note("file '" + r.name + "': chunk length mismatch");
        continue;
      }
      rep.bytes_checked += chunk.value().size();
      whole.update(chunk.value());
    }
    if (sum != r.logical_size) {
      note("file '" + r.name + "': chunk lengths sum to " + std::to_string(sum) + " but size is " +
           std::to_string(r.logical_size));
    }
    if (level == VerifyLevel::kDeep && whole.finish() != r.file_sha256) {
      note("file '" + r.name + "': whole-file digest mismatch");
    }
  }

  // Every index entry must point at a readable record. This catches the reverse
  // failure from the one above: an index that has drifted from the containers.
  if (level == VerifyLevel::kDeep) {
    for (size_t shard = 0; shard < index_.shard_count(); shard++) {
      std::vector<Location> locs;
      size_t cursor = 0;
      do {
        cursor = index_.scan_shard(shard, cursor, 256, [&](const Entry& e) {
          locs.push_back(Location::unpack(e.loc));
        });
        for (const Location& loc : locs) {
          auto reader = reader_for(loc.container_id);
          if (!reader.ok()) {
            note("index points at missing container " + std::to_string(loc.container_id));
            continue;
          }
          auto data = reader.value()->read(loc.offset, loc.length, nullptr);
          if (!data.ok()) note("index entry unreadable: " + data.status().message());
        }
        locs.clear();
      } while (cursor != 0);
    }
  }
  return rep;
}

}  // namespace dedupe
