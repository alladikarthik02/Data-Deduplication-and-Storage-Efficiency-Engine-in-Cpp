// The store: the engine itself.
//
// LAYOUT
//   <store>/STORE            superblock: magic, version, uuid, chunker params
//   <store>/LOCK             flock'd for the lifetime of an open Store (S14)
//   <store>/containers/*.dat append-only chunk records
//   <store>/recipes/*.rcp    one per ingested file
//
// RECIPE FILENAMES ARE HASHES, NOT NAMES.
//   A recipe is stored as sha256(name) in hex, with the real name inside the
//   file. The filesystem therefore never sees a single user-controlled byte, so
//   "../../etc/passwd", a 4000-character name, an embedded NUL and a name
//   containing '/' are all structurally impossible rather than defended against
//   (S12). Lookup by name stays O(1) -- hash it and open that file. The cost is
//   that `ls` must read every recipe to recover the names, which is O(files) and
//   only ever runs interactively.
//
// DURABILITY ORDER (S4), the whole reason ingest looks the way it does:
//   chunk -> append to container -> fsync CONTAINER -> publish RECIPE atomically.
//   The recipe is the only thing that makes a file exist. Publishing it before
//   the chunks are durable would mean a crash leaves a file that claims to exist
//   and cannot be read.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "dedupe/chunker.h"
#include "dedupe/container.h"
#include "dedupe/format.h"
#include "dedupe/index.h"
#include "dedupe/io.h"
#include "dedupe/status.h"
#include "dedupe/types.h"

namespace dedupe {

struct StoreOptions {
  ChunkerParams chunker;
  // S8: verify the chunk's full 256-bit fingerprint on every read, not just its
  // CRC. The index key is a 128-bit truncation, so this is what makes a
  // truncated-key collision impossible to act on rather than merely unlikely.
  // On by default: the cost is a memcmp against bytes already in cache.
  bool verify_fingerprint_on_read = true;
  size_t read_buffer_bytes = 1u << 20;
};

struct IngestStats {
  uint64_t logical_bytes = 0;
  uint64_t physical_bytes = 0;  // bytes actually appended to containers
  uint64_t chunks_total = 0;
  uint64_t chunks_new = 0;
  uint64_t chunks_deduped = 0;
  double dedup_ratio() const {
    return physical_bytes ? static_cast<double>(logical_bytes) / static_cast<double>(physical_bytes)
                          : 0.0;
  }
};

struct FileInfo {
  std::string name;
  uint64_t logical_size = 0;
  uint64_t chunk_count = 0;
};

struct StoreStats {
  uint64_t files = 0;
  uint64_t logical_bytes = 0;      // total bytes of all ingested files
  uint64_t physical_bytes = 0;     // total bytes of all container files
  uint64_t unique_chunks = 0;
  uint64_t chunk_references = 0;   // total chunk slots across all recipes
  uint64_t containers = 0;
  size_t index_bytes = 0;
  double dedup_ratio() const {
    return physical_bytes ? static_cast<double>(logical_bytes) / static_cast<double>(physical_bytes)
                          : 0.0;
  }
};

enum class VerifyLevel { kQuick, kDeep };

struct VerifyReport {
  uint64_t files_checked = 0;
  uint64_t chunks_checked = 0;
  uint64_t bytes_checked = 0;
  std::vector<std::string> problems;
  bool ok() const { return problems.empty(); }
};

class Store;

// One ingest stream. Owns its own active container so that appends from
// different threads never contend on a shared writer -- the design decision that
// removes the obvious global bottleneck (SPEC 3.5). The cost is space
// fragmentation across partially-filled containers, which GC's copy-forward
// compaction later reclaims (SPEC 8.2).
// A container that is currently open for appending, shared between the session
// that owns it and any OTHER session that needs to make its contents durable.
//
// This exists because of a cross-thread durability hole (CHALLENGES B11): thread
// B can dedup against a chunk thread A appended but never synced, then publish a
// recipe referencing it. Syncing only your own container makes S4 hold within a
// thread and fail across threads. A recipe publish must therefore be able to
// reach into whichever containers it referenced.
struct ContainerSlot {
  std::mutex mu;                     // guards writer_ (append, flush, sync, close)
  ContainerWriter writer;
  std::atomic<uint64_t> durable{0};  // bytes known to be fsynced
  std::atomic<uint64_t> visible{0};  // bytes handed to the kernel (readable)
  std::atomic<bool> open{true};
};

class IngestSession {
 public:
  ~IngestSession();
  IngestSession(IngestSession&&) noexcept;
  IngestSession& operator=(IngestSession&&) noexcept;
  IngestSession(const IngestSession&) = delete;
  IngestSession& operator=(const IngestSession&) = delete;

  Result<IngestStats> ingest_file(const std::string& name, const std::string& source_path);
  Result<IngestStats> ingest_bytes(const std::string& name, ByteSpan data);

  // Flush and release the active container. Called by the destructor too, but
  // the destructor cannot report an error, so callers who care call this.
  Status finish();

 private:
  friend class Store;
  IngestSession() = default;

  Result<Location> write_chunk(const Digest32& fp, ByteSpan payload);
  // Make every container this ingest referenced durable -- ours AND anyone
  // else's that we deduped against.
  Status make_referenced_durable(const std::map<uint32_t, uint64_t>& referenced);
  void note_reference(std::map<uint32_t, uint64_t>* referenced, const Location& loc);

  Store* store_ = nullptr;
  std::shared_ptr<ContainerSlot> slot_;
  uint64_t bytes_since_sync_ = 0;
};

class Store {
 public:
  ~Store();
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;

  // Opens an existing store or creates one. On open it recovers any torn
  // container tails and rebuilds the in-memory index by scanning containers --
  // the index is a cache, never ground truth (SPEC 3.0).
  static Result<std::unique_ptr<Store>> open(const std::string& path, StoreOptions opts = {});

  Result<IngestSession> begin_ingest();

  // Convenience wrappers: one session, one file.
  Result<IngestStats> ingest_file(const std::string& name, const std::string& source_path);
  Result<IngestStats> ingest_bytes(const std::string& name, ByteSpan data);

  Status restore(const std::string& name, const std::string& out_path);
  Result<std::vector<uint8_t>> restore_bytes(const std::string& name);

  Status remove(const std::string& name);
  Result<bool> exists(const std::string& name);
  Result<std::vector<FileInfo>> list();
  Result<StoreStats> stats();
  Result<VerifyReport> verify(VerifyLevel level);

  Result<Recipe> read_recipe(const std::string& name);
  Result<std::vector<Recipe>> all_recipes();

  const std::string& path() const { return path_; }
  const StoreOptions& options() const { return opts_; }
  Index& index() { return index_; }
  const std::array<uint8_t, 16>& uuid() const { return uuid_; }

  // How many times a recipe publish had to fsync a container it did not own,
  // because it deduped against a chunk another session had written but not yet
  // made durable. Observable so the cross-thread S4 fix can be tested rather
  // than asserted (CHALLENGES B11); also a genuine contention signal.
  uint64_t forced_syncs() const { return forced_syncs_.load(std::memory_order_relaxed); }

  // Reads one chunk by fingerprint, through the index. Verifies CRC always and
  // the full fingerprint when options().verify_fingerprint_on_read is set.
  Result<std::vector<uint8_t>> read_chunk(const Digest32& fp);

 private:
  friend class IngestSession;
  Store() = default;

  std::string containers_dir() const { return path_join(path_, "containers"); }
  std::string recipes_dir() const { return path_join(path_, "recipes"); }
  std::string container_path(uint32_t id) const;
  std::string recipe_path(const std::string& name) const;

  Status write_superblock();
  Status read_superblock();
  Status rebuild_index();

  Result<std::shared_ptr<ContainerSlot>> new_container();
  // Ensure a container's bytes up to `end` are readable (flushed) or durable
  // (flushed + fsynced). A container with no open slot was closed or predates
  // this process, so whatever is on disk is already both.
  Status ensure_visible(uint32_t container_id, uint64_t end);
  Status ensure_durable(uint32_t container_id, uint64_t end);
  std::shared_ptr<ContainerSlot> find_slot(uint32_t container_id);
  Result<std::shared_ptr<ContainerReader>> reader_for(uint32_t container_id);

  std::string path_;
  StoreOptions opts_;
  std::array<uint8_t, 16> uuid_{};
  FileLock lock_;
  Index index_;
  std::atomic<uint32_t> next_container_id_{0};

  // Open readers are shared and reference-counted so a container cannot be
  // unlinked from under an in-flight read -- the property GC will depend on (S2).
  std::shared_mutex readers_mu_;
  std::map<uint32_t, std::shared_ptr<ContainerReader>> readers_;

  std::shared_mutex slots_mu_;
  std::map<uint32_t, std::shared_ptr<ContainerSlot>> slots_;

  std::atomic<uint64_t> physical_bytes_{0};
  std::atomic<uint64_t> forced_syncs_{0};
};

}  // namespace dedupe
