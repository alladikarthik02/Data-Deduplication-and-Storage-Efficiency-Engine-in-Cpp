// The fingerprint index: fingerprint -> where that chunk lives.
//
// This is the structure résumé bullet R3.1 ("memory-efficient fingerprint index")
// and R3.2 ("throughput steady as the dataset grew") are about, so every choice
// below is made for one of those two reasons and is measured in bench_index.
//
// WHY OPEN ADDRESSING AND NOT std::unordered_map:
//   unordered_map allocates a NODE per entry (~48-64 B of payload plus allocator
//   header) and a bucket array of pointers on top. Every lookup chases a pointer
//   into cold memory. A flat array of 32-byte entries costs one cache miss for
//   the probe and usually resolves inside that same line. The claim is measured
//   against exactly that baseline rather than asserted.
//
// WHY 128-BIT TRUNCATED KEYS:
//   Halves the key cost against storing the full 256-bit fingerprint. The width
//   is computed, not chosen for looking round: by the birthday bound, 10^9 chunks
//   give ~10^18/2^129 ~= 10^-20 collision probability. A 64-bit key would NOT
//   have been safe -- at 10^8 chunks that is ~10^-3, a real corruption risk. And
//   a truncated-key collision is not silently trusted anyway: the chunk's own
//   header carries the full fingerprint and the reader verifies it (S8).
//
// WHY 256 SHARDS:
//   Two things at once. (1) Fine-grained locking: 256 independent shared_mutexes
//   mean ingest threads almost never contend. (2) No global rehash cliff: shards
//   grow independently, so a resize stalls 1/256th of the keyspace instead of the
//   whole engine. That is what makes "throughput steady as the dataset grows"
//   true rather than aspirational.
//
// WHY BACKWARD-SHIFT DELETION AND NOT TOMBSTONES:
//   GC erases entries, and a dedup store's delete traffic is not incidental.
//   Tombstones never go away on their own: they lengthen every probe chain until
//   the table is rebuilt, so a delete-heavy store slowly degrades. Backward-shift
//   deletion restores the table to exactly the state it would have had if the
//   erased key had never been inserted. It costs more per erase and nothing
//   afterwards.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "dedupe/format.h"
#include "dedupe/status.h"
#include "dedupe/types.h"

namespace dedupe {

// Where a chunk lives, packed into 64 bits (SPEC 3.4):
//   container_id : 16   -> 65536 containers
//   offset       : 27   -> exactly spans a 128 MiB container
//   length       : 17   -> covers the 64 KiB max chunk
// 65536 x 128 MiB = 8 TiB addressable, and the limits are checked at insert
// rather than discovered as a wrapped offset later.
struct Location {
  uint32_t container_id = 0;
  uint32_t offset = 0;
  uint32_t length = 0;

  static constexpr uint32_t kMaxContainer = (1u << 16) - 1;
  static constexpr uint32_t kMaxOffset = (1u << 27) - 1;
  static constexpr uint32_t kMaxLength = (1u << 17) - 1;

  bool fits() const {
    return container_id <= kMaxContainer && offset <= kMaxOffset && length <= kMaxLength;
  }
  uint64_t pack() const {
    return (static_cast<uint64_t>(container_id) << 44) | (static_cast<uint64_t>(offset) << 17) |
           static_cast<uint64_t>(length);
  }
  static Location unpack(uint64_t v) {
    Location l;
    l.container_id = static_cast<uint32_t>(v >> 44);
    l.offset = static_cast<uint32_t>((v >> 17) & kMaxOffset);
    l.length = static_cast<uint32_t>(v & kMaxLength);
    return l;
  }
  bool operator==(const Location& o) const {
    return container_id == o.container_id && offset == o.offset && length == o.length;
  }
};

// Exactly 32 bytes: two per 64-byte cache line. Asserted, not hoped for -- silent
// padding to 40 would make every memory figure in the spec wrong by 25%.
struct Entry {
  uint64_t k0 = 0;  // fingerprint bytes 0..7
  uint64_t k1 = 0;  // fingerprint bytes 8..15  (also the probe hash)
  uint64_t loc = 0;
  // Stamped on every write and every dedup hit. GC frees only entries whose
  // touch_epoch predates the start of its scan (SPEC 3.6). Atomic because that
  // stamp happens under a SHARED lock -- if it were a plain uint32_t, two
  // concurrent dedup hits on the same chunk would be a data race, and making the
  // hit path take an exclusive lock instead would put GC's whole reason for
  // existing (not blocking the foreground) back on the table.
  std::atomic<uint32_t> touch_epoch{0};
  // bit 0 = occupied, bits 1..31 = the GC epoch in which this entry was last
  // MARKED as reachable from a recipe. Packing the mark into the existing flags
  // word keeps the entry at 32 bytes; a separate field would have pushed it to
  // 40 and cost 25% more RAM for one bit of information per cycle.
  //
  // Atomic because GC marks under a SHARED lock: marking is not a structural
  // change, and taking the exclusive lock once per chunk reference would make
  // the mark phase stall every ingest thread -- exactly what S7 forbids.
  std::atomic<uint32_t> flags{0};

  static constexpr uint32_t kOccupied = 1u;

  Entry() = default;
  Entry(const Entry& o)
      : k0(o.k0), k1(o.k1), loc(o.loc),
        touch_epoch(o.touch_epoch.load(std::memory_order_relaxed)),
        flags(o.flags.load(std::memory_order_relaxed)) {}
  Entry& operator=(const Entry& o) {
    k0 = o.k0;
    k1 = o.k1;
    loc = o.loc;
    touch_epoch.store(o.touch_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
    flags.store(o.flags.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }
  bool occupied() const { return (flags.load(std::memory_order_relaxed) & kOccupied) != 0; }
  uint32_t mark_epoch() const { return flags.load(std::memory_order_relaxed) >> 1; }
  void set_mark_epoch(uint32_t e) {
    flags.store(kOccupied | ((e & 0x7FFFFFFFu) << 1), std::memory_order_relaxed);
  }
  void clear() {
    k0 = k1 = loc = 0;
    flags.store(0, std::memory_order_relaxed);
    touch_epoch.store(0, std::memory_order_relaxed);
  }
};
static_assert(sizeof(Entry) == 32, "index entry must stay 32 bytes (SPEC 3.4)");
static_assert(alignof(Entry) == 8, "index entry alignment");

struct InsertResult {
  bool inserted = false;  // false => an entry for this fingerprint already existed
  Location loc;           // the winner's location: ours if inserted, theirs if not
};

struct IndexStats {
  size_t entries = 0;
  size_t capacity = 0;     // total slots across all shards
  size_t memory_bytes = 0; // slots only; the shard headers are counted separately
  double load_factor = 0;
  size_t max_probe = 0;
};

class Index {
 public:
  static constexpr size_t kShards = 256;
  static constexpr double kMaxLoad = 0.90;   // safe at this load thanks to Robin Hood
  static constexpr double kGrowthFactor = 1.5;  // not 2.0 -- see CHALLENGES B10

  // 16 slots x 256 shards = a 128 KiB floor. Worth keeping small: a store with a
  // few hundred chunks should not report megabytes of index. Growth is 1.5x, so
  // the early rehashes are cheap and geometric.
  explicit Index(size_t initial_slots_per_shard = 16);
  Index(const Index&) = delete;
  Index& operator=(const Index&) = delete;

  // Look up a fingerprint. On a hit the entry is stamped with the current epoch,
  // which is what keeps a chunk that is being deduped against from being
  // reclaimed by a GC cycle already in flight (SPEC 3.6).
  bool lookup(const Digest32& fp, Location* out);

  // Insert, or report that someone else got there first. The loser of an ingest
  // race does NOT overwrite: two threads that chunked identical content wrote
  // two copies, and the second copy simply becomes garbage for GC to reclaim.
  // Overwriting would leave the index pointing at the newer copy while recipes
  // written moments earlier still reference the older one.
  Result<InsertResult> insert(const Digest32& fp, Location loc);

  // Point an existing fingerprint at a new location. This is GC's copy-forward
  // compaction step, and it must not create an entry if one is absent.
  bool repoint(const Digest32& fp, Location loc);

  // Like lookup(), but does NOT stamp touch_epoch. GC must use this: a scanning
  // pass that touched every entry it looked at would mark the whole store as
  // hot and reclaim nothing, forever.
  bool peek(const Digest32& fp, Location* out) const;

  // Mark an entry as reachable in `mark_epoch`. Shared lock; no touch stamp.
  bool mark(const Digest32& fp, uint32_t mark_epoch);

  // Conditional repoint for GC's copy-forward: move the entry only if it still
  // points where we think it does. A concurrent ingest may have already
  // repointed it, and clobbering that would strand the newer copy.
  bool repoint_if(const Digest32& fp, Location expect, Location to);

  bool erase(const Digest32& fp);
  // Erase by truncated key. GC's sweep works from index entries, which hold only
  // the 128-bit key -- and that is enough, since the shard is the low byte of k0
  // (fingerprint byte 0) and the probe hash is k1.
  bool erase_key(uint64_t k0, uint64_t k1);
  bool contains(const Digest32& fp);

  size_t size() const;
  IndexStats stats() const;
  size_t memory_bytes() const;

  uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }
  // Fences a GC cycle: everything stamped from now on is newer than the scan.
  uint32_t bump_epoch() { return epoch_.fetch_add(1, std::memory_order_acq_rel) + 1; }

  size_t shard_count() const { return kShards; }

  // Bounded iteration for GC (S7): visit at most `limit` slots of one shard,
  // starting at `cursor`, under one short shared lock. Returns the next cursor,
  // or 0 when the shard is exhausted. Callers loop; the lock is dropped between
  // batches so the foreground is never blocked for longer than one batch.
  template <class F>
  size_t scan_shard(size_t shard, size_t cursor, size_t limit, F&& fn) const {
    const Shard& s = shards_[shard];
    std::shared_lock<std::shared_mutex> lk(s.mu);
    size_t visited = 0;
    size_t i = cursor;
    for (; i < s.slots.size() && visited < limit; i++, visited++) {
      if (s.slots[i].occupied()) fn(s.slots[i]);
    }
    return (i >= s.slots.size()) ? 0 : i;
  }

 private:
  struct Shard {
    mutable std::shared_mutex mu;
    std::vector<Entry> slots;
    size_t cap = 0;  // NOT a power of two: growth is 1.5x, see grow()
    size_t count = 0;
    size_t grow_at = 0;
  };

  static size_t shard_of(const Digest32& fp) { return fp[0]; }
  static void key_of(const Digest32& fp, uint64_t* k0, uint64_t* k1) {
    *k0 = get_u64le(fp.data());
    *k1 = get_u64le(fp.data() + 8);
  }

  // Home slot via Lemire's fastrange: map a uniform 64-bit key onto [0, cap)
  // with one multiply, keeping the HIGH half. Works for any capacity, not just
  // powers of two -- which is what lets growth be 1.5x instead of 2x -- and it
  // consumes the high bits of the key, which are the best-distributed ones.
  //
  // The key is k1 (fingerprint bytes 8..15), deliberately NOT k0: k0's low byte
  // IS fp[0], which already chose the shard, so using it would correlate the
  // slot with the shard and waste part of the keyspace.
  static size_t home_of(uint64_t k1, size_t cap) {
    return static_cast<size_t>((static_cast<__uint128_t>(k1) * cap) >> 64);
  }
  static size_t next_index(size_t i, size_t cap) { return (i + 1 == cap) ? 0 : i + 1; }
  static size_t distance(size_t i, size_t home, size_t cap) {
    return (i >= home) ? (i - home) : (i + cap - home);
  }
  static size_t grow_threshold(size_t cap);

  // All of these assume the caller holds the appropriate shard lock.
  static bool find_slot(const Shard& s, uint64_t k0, uint64_t k1, size_t* out);
  static void grow(Shard& s);
  static void insert_unlocked(Shard& s, Entry e);  // by value: it gets shuffled
  static void erase_at(Shard& s, size_t idx);

  std::array<Shard, kShards> shards_;
  std::atomic<uint32_t> epoch_{1};
};

}  // namespace dedupe
