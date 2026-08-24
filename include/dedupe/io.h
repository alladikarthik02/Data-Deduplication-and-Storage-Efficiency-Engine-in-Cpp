// Checked filesystem I/O, with deliberate fault injection.
//
// TWO JOBS, BOTH FROM THE SAFETY TABLE:
//
//  1. S11 -- no silent failure. Every syscall's return is checked, every short
//     write is retried, and the results are [[nodiscard]]. `write()` returning 4095
//     when you asked for 4096 is not an error; it is a normal outcome that a naive
//     wrapper turns into one truncated byte on disk and no complaint.
//
//  2. S4 -- durability ordering. The engine must never publish a reference to data
//     that is not yet durable, which means fsync ordering is a correctness property.
//     A property you cannot fail on demand is a property you cannot test, so this
//     layer can be told to fail the Nth write / fsync / rename and the engine must
//     survive it (SPEC 6, "fault injection").
#pragma once

#include <fcntl.h>
#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "dedupe/status.h"
#include "dedupe/types.h"

namespace dedupe {

namespace faults {

// The syscall classes the engine depends on. Anything that can fail and whose
// failure has a distinct recovery path gets its own entry.
enum class Op : uint8_t {
  kOpen = 0,
  kWrite,
  kPWrite,
  kPRead,
  kFsync,
  kFsyncDir,
  kRename,
  kTruncate,
  kUnlink,
  kMax,
};

const char* op_name(Op op);

// TEST ONLY. Arm the `nth` (1-based, counted from the last reset) call to `op` to
// fail with `code` instead of performing the operation.
void fail_nth(Op op, uint64_t nth, Err code = Err::kIo);

// TEST ONLY. Fail the `nth` call to `op` AND every call after it. Models a device
// that has gone away rather than a single transient error -- the difference
// matters, because a one-shot failure lets the next retry succeed and paper over
// the very state a recovery test is trying to create.
void fail_from(Op op, uint64_t nth, Err code = Err::kIo);

// TEST ONLY. Arm the `nth` call to write/pwrite to transfer only half its bytes
// and report success -- the short-write case a naive wrapper silently truncates on.
void short_nth(Op op, uint64_t nth);

// Clear all armed faults and zero all counters.
void reset();

// How many times `op` has been attempted since the last reset.
uint64_t count(Op op);

}  // namespace faults

// An owned file descriptor. Move-only: two owners would mean a double close, and
// a closed-then-reused fd is the kind of bug that corrupts an unrelated file.
class File {
 public:
  File() = default;
  ~File();
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  static Result<File> open(const std::string& path, int flags, mode_t mode = 0644);

  bool is_open() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  const std::string& path() const { return path_; }

  // Appends `data` at the current offset, looping until every byte is written.
  // `out_done`, if given, receives the number of bytes that DID reach the file --
  // including when this returns an error. Callers that buffer need that: after a
  // partial write the file holds a prefix, and re-sending the whole buffer would
  // duplicate it (CHALLENGES B12).
  Status write_all(ByteSpan data, size_t* out_done = nullptr);
  // Writes at an absolute offset, looping until every byte is written.
  Status pwrite_all(ByteSpan data, uint64_t offset);

  // Reads exactly `n` bytes; a short read (i.e. EOF) is an error, because every
  // caller of this knows how long the record it wants is.
  Status pread_exact(uint8_t* buf, size_t n, uint64_t offset);
  // Reads up to `n` bytes; returns how many. Used by the recovery scanner, which
  // does not know where the file ends.
  Result<size_t> pread_upto(uint8_t* buf, size_t n, uint64_t offset);

  Status sync();  // fsync: the data is on the device (see SPEC 8.6 for the caveat)
  Status truncate(uint64_t length);
  Result<uint64_t> size() const;
  Status close();

 private:
  int fd_ = -1;
  std::string path_;
};

// Directory and path operations.
//
// fsync_dir matters more than it looks: a rename is only durable once the
// DIRECTORY has been synced. Syncing the file and then renaming it leaves a
// window where a crash loses the name even though the bytes survived -- which,
// for a recipe, means an ingest that reported success has vanished (S4).
Status fsync_dir(const std::string& path);
Status make_dirs(const std::string& path);
Status rename_file(const std::string& from, const std::string& to);
Status unlink_file(const std::string& path);
Result<bool> path_exists(const std::string& path);
Result<std::vector<std::string>> list_dir(const std::string& path);
Result<uint64_t> file_size(const std::string& path);

// Write a whole file atomically: tmp -> fsync -> rename -> fsync(dir).
// A reader therefore sees either the old contents or the new ones, never a
// half-written file. This is the publish step for recipes (SPEC 3.3).
Status write_file_atomic(const std::string& path, ByteSpan data);

Result<std::vector<uint8_t>> read_file(const std::string& path);

// An exclusive advisory lock held for the lifetime of the object (SPEC S14).
// flock is released automatically if the process dies, which is what we want:
// a crashed process must not lock the store forever.
class FileLock {
 public:
  FileLock() = default;
  ~FileLock();
  FileLock(FileLock&&) noexcept;
  FileLock& operator=(FileLock&&) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  static Result<FileLock> acquire(const std::string& path);
  bool held() const { return file_.is_open(); }

 private:
  File file_;
};

// Join two path components. Deliberately dumb: no normalization, no symlink
// resolution. Path *safety* is enforced by validate_component() below, not by
// trying to canonicalize hostile input.
std::string path_join(const std::string& a, const std::string& b);

// SPEC S12: a name that reaches the filesystem must not be able to escape the
// store directory. Rejects empty, ".", "..", anything containing '/' or a NUL,
// and anything over 255 bytes.
Status validate_component(const std::string& name);

}  // namespace dedupe
