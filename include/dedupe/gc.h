// Background space reclamation -- résumé bullet #2, and the hardest correctness
// problem in this engine.
//
// WHY NOT REFERENCE COUNTING.
//   Refcounts race. A chunk's count can reach zero exactly while another thread
//   is mid-ingest and about to dedup-hit that chunk; the window between "count
//   hit 0" and "chunk deleted" is a data-loss window, and closing it needs a lock
//   the ingest hot path would have to take on EVERY chunk. Refcounts also cannot
//   self-heal: one lost decrement leaks forever, one lost increment corrupts.
//
// WHAT WE DO: EPOCH-BASED CONSERVATIVE MARK-AND-SWEEP.
//
//   Phase 0  FENCE    epoch_start = ++global_epoch.
//                     From here on, every write and every dedup HIT stamps its
//                     index entry with an epoch >= epoch_start.
//   Phase 1  MARK     walk every recipe on disk; stamp each referenced entry
//                     with mark_epoch = epoch_start.
//   Phase 2  SWEEP    erase entries that are unmarked AND untouched since the
//                     fence.
//   Phase 3  COMPACT  a container with no live chunks is unlinked outright; one
//                     below the live-fraction threshold has its live chunks
//                     copied forward into a fresh container, and only then is
//                     the original unlinked.
//
// THE SAFETY ARGUMENT (S2), stated so it can be attacked.
//   A chunk is freed only if no recipe references it AND nothing touched it since
//   the fence. A recipe published mid-scan may be missed by MARK -- but every
//   chunk it references was either newly written (stamped) or a dedup hit
//   (lookup stamps on hit, deliberately), so the epoch clause saves it. The
//   failure mode of this design is RETAINING GARBAGE for one extra cycle, never
//   freeing live data. Conservatism is aimed at the direction where a bug costs
//   space rather than data.
//
// NOT BLOCKING THE FOREGROUND (S7).
//   MARK takes a shared lock per chunk reference and never an exclusive one --
//   marking is not a structural change. SWEEP walks each shard in bounded
//   batches, dropping the lock between them. COMPACT does all its I/O with no
//   index lock held; it takes a short exclusive lock per repointed chunk. GC
//   never holds two shard locks at once and never holds one across I/O.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "dedupe/status.h"
#include "dedupe/store.h"

namespace dedupe {

struct GcOptions {
  // Compact a container when its live fraction falls below this. 0.5 means "at
  // most half the bytes are wasted before we pay to rewrite it" -- copy-forward
  // costs a read and a write of everything still live, so compacting a
  // 90%-live container would spend far more I/O than it reclaims.
  double compact_below_live_fraction = 0.5;
  // Index slots visited per shard-lock acquisition. The knob that bounds how
  // long GC can make an ingest thread wait (S7).
  size_t scan_batch = 256;
  bool enable_compaction = true;
  std::chrono::milliseconds interval{2000};  // background thread period
};

struct GcStats {
  uint32_t epoch = 0;       // the fence
  uint32_t safe_epoch = 0;  // reclaim watermark: min(fence, oldest in-flight ingest)
  uint64_t recipes_scanned = 0;
  uint64_t marked = 0;
  uint64_t swept = 0;                 // index entries erased
  uint64_t containers_examined = 0;
  uint64_t containers_deleted = 0;    // had no live chunks at all
  uint64_t containers_compacted = 0;  // rewritten to drop dead chunks
  uint64_t chunks_copied = 0;
  uint64_t bytes_before = 0;
  uint64_t bytes_after = 0;
  double seconds = 0;
  int64_t bytes_reclaimed() const {
    return static_cast<int64_t>(bytes_before) - static_cast<int64_t>(bytes_after);
  }
};

class Gc {
 public:
  Gc(Store* store, GcOptions opts = {});
  ~Gc();
  Gc(const Gc&) = delete;
  Gc& operator=(const Gc&) = delete;

  // One complete cycle, synchronously. Deterministic, which is what the tests
  // need; the background thread is just this in a loop.
  Result<GcStats> run_once();

  void start();  // background thread, every opts.interval
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }

  // Cycles completed since construction. Lets a test wait for the background
  // thread to have actually done something rather than sleeping and hoping.
  uint64_t cycles() const { return cycles_.load(std::memory_order_acquire); }
  const GcOptions& options() const { return opts_; }

 private:
  Result<uint64_t> mark_phase(uint32_t epoch, GcStats* stats);
  Result<uint64_t> sweep_phase(uint32_t epoch, uint32_t safe_epoch, GcStats* stats);
  Status compact_phase(GcStats* stats);
  Status compact_one(uint32_t id, const ScanResult& scan, uint64_t live_bytes, GcStats* stats);

  Store* store_;
  GcOptions opts_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> cycles_{0};
};

}  // namespace dedupe
