#include "dedupe/index.h"

#include <bit>

namespace dedupe {

Index::Index(size_t initial_slots_per_shard) {
  const size_t cap = initial_slots_per_shard < 8 ? 8 : initial_slots_per_shard;
  for (auto& s : shards_) {
    s.slots.assign(cap, Entry{});
    s.cap = cap;
    s.count = 0;
    s.grow_at = grow_threshold(cap);
  }
}

size_t Index::grow_threshold(size_t cap) {
  size_t t = static_cast<size_t>(static_cast<double>(cap) * kMaxLoad);
  // At least one slot must always stay empty, or an unsuccessful probe would
  // loop forever. This is the kind of invariant that never fires in testing and
  // hangs the process in production.
  if (t >= cap) t = cap - 1;
  return t;
}

// ROBIN HOOD probing.
//
// Plain linear probing at load a costs ~(1 + 1/(1-a)^2)/2 probes for an
// UNSUCCESSFUL lookup -- 50 probes at a=0.9. Unsuccessful is the common case
// here: every genuinely new chunk misses. Robin Hood fixes that by keeping
// entries ordered by displacement, so a probe can stop the moment it meets an
// entry closer to its own home than we are to ours. Cost drops to ~1/(1-a), and
// the variance collapses, which is what turns a long tail into a bounded one.
//
// The name: on insert, an entry that has travelled far "steals" a slot from one
// that has travelled less. Rich and poor end up equalized.
bool Index::find_slot(const Shard& s, uint64_t k0, uint64_t k1, size_t* out) {
  size_t i = home_of(k1, s.cap);
  size_t dist = 0;
  while (true) {
    const Entry& e = s.slots[i];
    if (!e.occupied()) {
      *out = i;
      return false;
    }
    if (e.k0 == k0 && e.k1 == k1) {
      *out = i;
      return true;
    }
    // The Robin Hood early exit: this slot's occupant is closer to home than we
    // are, so our key cannot be any further along -- it would have stolen this
    // slot when it was inserted.
    if (distance(i, home_of(e.k1, s.cap), s.cap) < dist) {
      *out = i;
      return false;
    }
    i = next_index(i, s.cap);
    dist++;
  }
}

void Index::insert_unlocked(Shard& s, Entry e) {
  size_t i = home_of(e.k1, s.cap);
  size_t dist = 0;
  while (true) {
    Entry& cur = s.slots[i];
    if (!cur.occupied()) {
      cur = e;
      s.count++;
      return;
    }
    if (cur.k0 == e.k0 && cur.k1 == e.k1) {
      cur = e;  // replace in place
      return;
    }
    const size_t d = distance(i, home_of(cur.k1, s.cap), s.cap);
    if (d < dist) {
      // We have travelled further than the current occupant: take the slot and
      // carry on placing the one we displaced.
      Entry tmp = cur;
      cur = e;
      e = tmp;
      dist = d;
    }
    i = next_index(i, s.cap);
    dist++;
  }
}

// GROWTH FACTOR 1.5, NOT 2.
//
// With doubling, a table's load oscillates between max/2 and max, so the average
// occupancy is only ~0.68 even with a 0.9 ceiling -- and the aggregate does not
// smooth out across shards, because uniform hashing makes all 256 shards cross
// their threshold within a few percent of each other. Measured, that was 72.9
// bytes per entry against a theoretical 35.6. Growing by 1.5 narrows the band to
// [0.6, 0.9] and takes the average to ~0.74. See CHALLENGES B10.
//
// The cost is that capacity is no longer a power of two, so the home slot is
// computed with Lemire's fastrange -- a 64x64 multiply keeping the high half --
// instead of a mask. That also uses the HIGH bits of the key, which is strictly
// better than the low bits a mask would take.
void Index::grow(Shard& s) {
  std::vector<Entry> old;
  old.swap(s.slots);
  size_t cap = old.size() + old.size() / 2;
  if (cap < 8) cap = 8;
  s.slots.assign(cap, Entry{});
  s.cap = cap;
  s.count = 0;
  s.grow_at = grow_threshold(cap);
  for (const Entry& e : old) {
    if (e.occupied()) insert_unlocked(s, e);
  }
}

// Backward-shift deletion, the Robin Hood variant.
//
// Blanking a slot would break every key whose probe chain runs THROUGH it: those
// keys become unreachable while still occupying space. In a dedup index that is
// not a crash -- the store still restores correctly, it just silently stops
// deduplicating against data it already holds. A bug that costs only disk is
// exactly the kind that survives to production.
//
// Tombstones would avoid it but never go away, lengthening every probe until the
// table is rebuilt. Shifting back restores the table to exactly the state it
// would have had if the erased key had never been inserted. Under Robin Hood the
// rule is simple: pull back any following entry that is not already at home.
void Index::erase_at(Shard& s, size_t idx) {
  size_t j = idx;
  while (true) {
    const size_t k = next_index(j, s.cap);
    if (!s.slots[k].occupied()) break;
    if (distance(k, home_of(s.slots[k].k1, s.cap), s.cap) == 0) break;
    s.slots[j] = s.slots[k];
    j = k;
  }
  s.slots[j].clear();
  s.count--;
}

bool Index::lookup(const Digest32& fp, Location* out) {
  Shard& s = shards_[shard_of(fp)];
  uint64_t k0, k1;
  key_of(fp, &k0, &k1);

  std::shared_lock<std::shared_mutex> lk(s.mu);
  size_t idx = 0;
  if (!find_slot(s, k0, k1, &idx)) return false;
  if (out != nullptr) *out = Location::unpack(s.slots[idx].loc);
  // Stamp the hit. A shared lock suffices because this is an atomic store to a
  // slot whose identity cannot change while any shared lock is held -- every
  // structural change takes the exclusive lock.
  s.slots[idx].touch_epoch.store(epoch(), std::memory_order_relaxed);
  return true;
}

Result<InsertResult> Index::insert(const Digest32& fp, Location loc) {
  if (!loc.fits()) {
    return Status::invalid("index: location does not fit the packed fields (container " +
                           std::to_string(loc.container_id) + ", offset " +
                           std::to_string(loc.offset) + ", length " + std::to_string(loc.length) +
                           ")");
  }
  Shard& s = shards_[shard_of(fp)];
  uint64_t k0, k1;
  key_of(fp, &k0, &k1);

  std::unique_lock<std::shared_mutex> lk(s.mu);
  size_t idx = 0;
  if (find_slot(s, k0, k1, &idx)) {
    InsertResult r;
    r.inserted = false;
    r.loc = Location::unpack(s.slots[idx].loc);
    // Still a touch: someone is actively using this chunk, so GC must not treat
    // it as cold (SPEC 3.6).
    s.slots[idx].touch_epoch.store(epoch(), std::memory_order_relaxed);
    return r;
  }
  if (s.count + 1 > s.grow_at) grow(s);

  Entry e;
  e.k0 = k0;
  e.k1 = k1;
  e.loc = loc.pack();
  e.flags = Entry::kOccupied;
  e.touch_epoch.store(epoch(), std::memory_order_relaxed);
  insert_unlocked(s, e);

  InsertResult r;
  r.inserted = true;
  r.loc = loc;
  return r;
}

bool Index::repoint(const Digest32& fp, Location loc) {
  if (!loc.fits()) return false;
  Shard& s = shards_[shard_of(fp)];
  uint64_t k0, k1;
  key_of(fp, &k0, &k1);
  std::unique_lock<std::shared_mutex> lk(s.mu);
  size_t idx = 0;
  if (!find_slot(s, k0, k1, &idx)) return false;
  s.slots[idx].loc = loc.pack();
  return true;
}

bool Index::erase(const Digest32& fp) {
  Shard& s = shards_[shard_of(fp)];
  uint64_t k0, k1;
  key_of(fp, &k0, &k1);
  std::unique_lock<std::shared_mutex> lk(s.mu);
  size_t idx = 0;
  if (!find_slot(s, k0, k1, &idx)) return false;
  erase_at(s, idx);
  return true;
}

bool Index::contains(const Digest32& fp) { return lookup(fp, nullptr); }

size_t Index::size() const {
  size_t n = 0;
  for (const auto& s : shards_) {
    std::shared_lock<std::shared_mutex> lk(s.mu);
    n += s.count;
  }
  return n;
}

size_t Index::memory_bytes() const {
  size_t n = 0;
  for (const auto& s : shards_) {
    std::shared_lock<std::shared_mutex> lk(s.mu);
    n += s.slots.capacity() * sizeof(Entry);
  }
  return n;
}

IndexStats Index::stats() const {
  IndexStats st;
  for (const auto& s : shards_) {
    std::shared_lock<std::shared_mutex> lk(s.mu);
    st.entries += s.count;
    st.capacity += s.slots.size();
    st.memory_bytes += s.slots.capacity() * sizeof(Entry);
    // Longest displacement actually in use: the honest measure of table health,
    // and the first thing to degrade under a bad hash or an overfull table.
    for (size_t i = 0; i < s.slots.size(); i++) {
      if (!s.slots[i].occupied()) continue;
      const size_t d = distance(i, home_of(s.slots[i].k1, s.cap), s.cap);
      if (d > st.max_probe) st.max_probe = d;
    }
  }
  st.load_factor = st.capacity ? static_cast<double>(st.entries) / static_cast<double>(st.capacity)
                               : 0.0;
  return st;
}

}  // namespace dedupe
