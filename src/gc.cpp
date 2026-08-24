#include "dedupe/gc.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "dedupe/gc_journal.h"

namespace dedupe {

Gc::Gc(Store* store, GcOptions opts) : store_(store), opts_(opts) {}

Gc::~Gc() { stop(); }

// ---------------------------------------------------------------------------
// Phase 1 -- MARK
// ---------------------------------------------------------------------------
Result<uint64_t> Gc::mark_phase(uint32_t epoch, GcStats* stats) {
  ASSIGN_OR_RETURN(std::vector<Recipe> recipes, store_->all_recipes());
  uint64_t marked = 0;
  for (const Recipe& r : recipes) {
    stats->recipes_scanned++;
    for (const RecipeEntry& e : r.entries) {
      // Shared lock, no touch stamp. Marking is not a structural change, so it
      // does not need to exclude readers -- and taking the exclusive lock once
      // per chunk reference would stall every ingest thread for the whole mark
      // phase, which is precisely what S7 forbids.
      if (store_->index().mark(e.fp, epoch)) marked++;
    }
  }
  stats->marked = marked;
  return marked;
}

// ---------------------------------------------------------------------------
// Phase 2 -- SWEEP
// ---------------------------------------------------------------------------
Result<uint64_t> Gc::sweep_phase(uint32_t epoch, uint32_t safe_epoch, GcStats* stats) {
  uint64_t swept = 0;
  Index& index = store_->index();

  for (size_t shard = 0; shard < index.shard_count(); shard++) {
    size_t cursor = 0;
    do {
      // Collect candidates under a SHORT shared lock, then erase them outside
      // it. Erasing during the walk would mean holding the exclusive lock across
      // the whole shard and, worse, mutating the table we are iterating.
      std::vector<std::pair<uint64_t, uint64_t>> doomed;
      cursor = index.scan_shard(shard, cursor, opts_.scan_batch, [&](const Entry& e) {
        const bool unmarked = e.mark_epoch() != epoch;
        // NOTE the watermark, not the fence. An ingest already in flight when
        // the fence was taken may have looked this chunk up (stamping it with
        // the OLD epoch) and not yet published the recipe that would mark it.
        // Holding the watermark back to the oldest in-flight ingest is what
        // makes S2 hold -- see CHALLENGES B13, where this exact race destroyed
        // live files under load.
        const bool cold = e.touch_epoch.load(std::memory_order_relaxed) < safe_epoch;
        if (unmarked && cold) doomed.emplace_back(e.k0, e.k1);
      });
      for (const auto& [k0, k1] : doomed) {
        if (index.erase_key(k0, k1)) swept++;
      }
    } while (cursor != 0);
  }
  stats->swept = swept;
  return swept;
}

// ---------------------------------------------------------------------------
// Phase 3 -- COMPACT
// ---------------------------------------------------------------------------
Status Gc::compact_one(uint32_t id, const ScanResult& scan, uint64_t live_bytes,
                       GcStats* stats) {
  // Which records are still live, and where they are going.
  struct Move {
    Digest32 fp;
    uint32_t src_offset;
    uint32_t length;
    uint32_t dst_offset = 0;
  };
  std::vector<Move> moves;
  for (const ScanRecord& rec : scan.records) {
    Location loc;
    // peek(), never lookup(): a scanning pass that stamped every entry it read
    // would mark the entire store as hot and reclaim nothing, forever.
    if (!store_->index().peek(rec.fp, &loc)) continue;
    if (loc.container_id != id || loc.offset != rec.offset) continue;  // a newer copy won
    moves.push_back({rec.fp, rec.offset, rec.length, 0});
  }
  if (moves.empty()) return Status();  // nothing to move; caller handles deletion

  ASSIGN_OR_RETURN(ContainerWriter dst, store_->create_container_for_gc());
  const uint32_t dst_id = dst.id();
  RETURN_IF_ERROR(store_->journal().append(GcOp::kBeginCompact, id, dst_id));

  ASSIGN_OR_RETURN(std::shared_ptr<ContainerReader> src, store_->reader(id));
  for (Move& m : moves) {
    ASSIGN_OR_RETURN(std::vector<uint8_t> payload, src->read(m.src_offset, m.length, &m.fp));
    ASSIGN_OR_RETURN(m.dst_offset, dst.append(m.fp, payload));
    stats->chunks_copied++;
  }
  // The destination must be complete and durable before anything points at it,
  // and long before the source is unlinked.
  RETURN_IF_ERROR(dst.sync());
  RETURN_IF_ERROR(dst.close());

  // THE decision point. Once this record is durable, dst is authoritative and
  // src is redundant; recovery will finish the job even if we die here.
  RETURN_IF_ERROR(store_->journal().append(GcOp::kCommit, id, dst_id));

  for (const Move& m : moves) {
    Location from;
    from.container_id = id;
    from.offset = m.src_offset;
    from.length = m.length;
    Location to;
    to.container_id = dst_id;
    to.offset = m.dst_offset;
    to.length = m.length;
    // Conditional: if a concurrent ingest already repointed this fingerprint at
    // a copy of its own, leave it alone. Clobbering it would strand that copy.
    (void)store_->index().repoint_if(m.fp, from, to);
  }

  RETURN_IF_ERROR(store_->retire_container(id));
  RETURN_IF_ERROR(store_->journal().append(GcOp::kDone, id, dst_id));

  stats->containers_compacted++;
  stats->bytes_before += scan.valid_bytes;
  stats->bytes_after += live_bytes + ContainerHeader::kSize +
                        static_cast<uint64_t>(moves.size()) * ChunkHeader::kSize;
  return Status();
}

Status Gc::compact_phase(GcStats* stats) {
  ASSIGN_OR_RETURN(std::vector<uint32_t> ids, store_->container_ids());
  for (uint32_t id : ids) {
    // Never compact a container an ingest session still owns: its length is
    // still moving, and its owner would keep appending into a file we are about
    // to unlink.
    if (store_->container_is_open(id)) continue;

    auto reader = store_->reader(id);
    if (!reader.ok()) continue;  // vanished under us; nothing to do
    auto scanned = reader.value()->scan();
    if (!scanned.ok()) continue;
    const ScanResult& scan = scanned.value();
    stats->containers_examined++;

    // Defence in depth for the class of bug in CHALLENGES B15: a container that
    // scans as empty is either genuinely empty or something lied to us about its
    // size. Deleting it would be irreversible, and skipping it costs one wasted
    // container until the next cycle. Wrong in the direction that costs space.
    if (scan.records.empty()) continue;

    uint64_t live_bytes = 0;
    for (const ScanRecord& rec : scan.records) {
      Location loc;
      if (!store_->index().peek(rec.fp, &loc)) continue;
      if (loc.container_id == id && loc.offset == rec.offset) live_bytes += rec.length;
    }

    if (live_bytes == 0) {
      // Nothing in here is referenced any more. No copy needed -- just record
      // the intent, unlink, and record completion.
      RETURN_IF_ERROR(store_->journal().append(GcOp::kDeleteContainer, id, 0));
      RETURN_IF_ERROR(store_->retire_container(id));
      RETURN_IF_ERROR(store_->journal().append(GcOp::kDone, id, 0));
      stats->containers_deleted++;
      stats->bytes_before += scan.valid_bytes;
      continue;
    }
    if (!opts_.enable_compaction) continue;

    const double live_fraction =
        scan.valid_bytes ? static_cast<double>(live_bytes) / static_cast<double>(scan.valid_bytes)
                         : 1.0;
    if (live_fraction >= opts_.compact_below_live_fraction) continue;
    RETURN_IF_ERROR(compact_one(id, scan, live_bytes, stats));
  }
  return Status();
}

// ---------------------------------------------------------------------------
// A cycle
// ---------------------------------------------------------------------------
Result<GcStats> Gc::run_once() {
  const auto t0 = std::chrono::steady_clock::now();
  GcStats stats;

  // Phase 0 -- THE FENCE, plus the reclaim watermark.
  //
  // Everything written or dedup-hit from this instant on carries an epoch >= the
  // fence and is ineligible for this cycle. But an ingest that was ALREADY
  // RUNNING may have stamped chunks with an older epoch and not yet published
  // the recipe that would mark them, so the watermark is held back to the oldest
  // in-flight ingest. Both are taken under one lock: separately, an ingest could
  // read the old epoch, GC could fence and see no active ingests, and only then
  // would the ingest register.
  uint32_t epoch = 0;
  const uint32_t safe_epoch = store_->fence_and_safe_epoch(&epoch);
  stats.epoch = epoch;
  stats.safe_epoch = safe_epoch;

  auto marked = mark_phase(epoch, &stats);
  RETURN_IF_ERROR(marked.status());
  auto swept = sweep_phase(epoch, safe_epoch, &stats);
  RETURN_IF_ERROR(swept.status());
  RETURN_IF_ERROR(compact_phase(&stats));
  RETURN_IF_ERROR(store_->journal().reset());  // nothing is in flight

  stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  cycles_.fetch_add(1, std::memory_order_release);
  return stats;
}

void Gc::start() {
  if (running_.exchange(true)) return;
  stop_.store(false);
  thread_ = std::thread([this] {
    while (!stop_.load(std::memory_order_acquire)) {
      (void)run_once();
      // Sleep in slices so stop() is responsive without a condition variable.
      const auto deadline = std::chrono::steady_clock::now() + opts_.interval;
      while (!stop_.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
  });
}

void Gc::stop() {
  if (!running_.load(std::memory_order_acquire)) return;
  stop_.store(true, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
  running_.store(false, std::memory_order_release);
}

}  // namespace dedupe
