#include "dedupe/io.h"

#include <dirent.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>

#include <algorithm>
#include <array>
#include <atomic>

namespace dedupe {
namespace faults {
namespace {

constexpr size_t kOpCount = static_cast<size_t>(Op::kMax);

// One counter and one armed trigger per op. Atomic so the concurrency suites can
// run under TSan without the injector itself being the race.
struct State {
  std::array<std::atomic<uint64_t>, kOpCount> counts{};
  std::array<std::atomic<uint64_t>, kOpCount> fail_at{};   // 0 = disarmed
  std::array<std::atomic<uint64_t>, kOpCount> fail_from{}; // 0 = disarmed
  std::array<std::atomic<uint64_t>, kOpCount> short_at{};  // 0 = disarmed
  std::array<std::atomic<uint64_t>, kOpCount> abort_at{};  // 0 = disarmed
  std::array<std::atomic<int>, kOpCount> fail_code{};
};

State& state() {
  static State s;
  return s;
}

}  // namespace

const char* op_name(Op op) {
  switch (op) {
    case Op::kOpen: return "open";
    case Op::kWrite: return "write";
    case Op::kPWrite: return "pwrite";
    case Op::kPRead: return "pread";
    case Op::kFsync: return "fsync";
    case Op::kFsyncDir: return "fsync_dir";
    case Op::kRename: return "rename";
    case Op::kTruncate: return "truncate";
    case Op::kUnlink: return "unlink";
    case Op::kMax: break;
  }
  return "?";
}

void fail_nth(Op op, uint64_t nth, Err code) {
  const size_t i = static_cast<size_t>(op);
  state().fail_at[i].store(nth, std::memory_order_relaxed);
  state().fail_code[i].store(static_cast<int>(code), std::memory_order_relaxed);
}

void fail_from(Op op, uint64_t nth, Err code) {
  const size_t i = static_cast<size_t>(op);
  state().fail_from[i].store(nth, std::memory_order_relaxed);
  state().fail_code[i].store(static_cast<int>(code), std::memory_order_relaxed);
}

void abort_at(Op op, uint64_t nth) {
  state().abort_at[static_cast<size_t>(op)].store(nth, std::memory_order_relaxed);
}

void short_nth(Op op, uint64_t nth) {
  state().short_at[static_cast<size_t>(op)].store(nth, std::memory_order_relaxed);
}

void reset() {
  for (size_t i = 0; i < kOpCount; i++) {
    state().counts[i].store(0, std::memory_order_relaxed);
    state().fail_at[i].store(0, std::memory_order_relaxed);
    state().fail_from[i].store(0, std::memory_order_relaxed);
    state().short_at[i].store(0, std::memory_order_relaxed);
    state().abort_at[i].store(0, std::memory_order_relaxed);
    state().fail_code[i].store(0, std::memory_order_relaxed);
  }
}

uint64_t count(Op op) {
  return state().counts[static_cast<size_t>(op)].load(std::memory_order_relaxed);
}

namespace {

// Called once per attempted operation. Returns an error if this call is the armed
// one. On the uninjected path this is one relaxed atomic increment and one
// compare -- immaterial next to the syscall it precedes.
Status tick(Op op) {
  const size_t i = static_cast<size_t>(op);
  const uint64_t n = state().counts[i].fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t kill = state().abort_at[i].load(std::memory_order_relaxed);
  if (kill != 0 && n >= kill) {
    // Die here, before the operation happens. No unwinding, no flush -- a crash.
    ::_exit(9);
  }
  const uint64_t from = state().fail_from[i].load(std::memory_order_relaxed);
  const uint64_t at = state().fail_at[i].load(std::memory_order_relaxed);
  if ((at != 0 && n == at) || (from != 0 && n >= from)) {
    const Err code = static_cast<Err>(state().fail_code[i].load(std::memory_order_relaxed));
    return Status::error(code, std::string("injected fault on ") + op_name(op) + " #" +
                                   std::to_string(n));
  }
  return Status();
}

bool tick_short(Op op) {
  const size_t i = static_cast<size_t>(op);
  const uint64_t at = state().short_at[i].load(std::memory_order_relaxed);
  return at != 0 && state().counts[i].load(std::memory_order_relaxed) == at;
}

}  // namespace
}  // namespace faults

// ---------------------------------------------------------------------------
// File
// ---------------------------------------------------------------------------

File::~File() {
  if (fd_ >= 0) ::close(fd_);
}

File::File(File&& o) noexcept : fd_(o.fd_), path_(std::move(o.path_)) { o.fd_ = -1; }

File& File::operator=(File&& o) noexcept {
  if (this != &o) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = o.fd_;
    path_ = std::move(o.path_);
    o.fd_ = -1;
  }
  return *this;
}

Result<File> File::open(const std::string& path, int flags, mode_t mode) {
  RETURN_IF_ERROR(faults::tick(faults::Op::kOpen));
  const int fd = ::open(path.c_str(), flags, mode);
  if (fd < 0) {
    if (errno == ENOENT) return Status::error(Err::kNotFound, "open " + path, errno);
    if (errno == EEXIST) return Status::error(Err::kExists, "open " + path, errno);
    return Status::from_errno("open " + path);
  }
  File f;
  f.fd_ = fd;
  f.path_ = path;
  return f;
}

Status File::write_all(ByteSpan data, size_t* out_done) {
  size_t done = 0;
  // Report progress on EVERY exit path, error included.
  struct Reporter {
    size_t* out;
    const size_t* done;
    ~Reporter() { if (out != nullptr) *out = *done; }
  } reporter{out_done, &done};

  while (done < data.size()) {
    RETURN_IF_ERROR(faults::tick(faults::Op::kWrite));
    size_t want = data.size() - done;
    // Injected short write: transfer half and report success, exactly as the
    // kernel is permitted to do. If the retry loop below were missing, this
    // would silently truncate the record.
    if (faults::tick_short(faults::Op::kWrite)) want = std::max<size_t>(1, want / 2);
    const ssize_t n = ::write(fd_, data.data() + done, want);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::from_errno("write " + path_);
    }
    if (n == 0) return Status::error(Err::kIo, "write returned 0 on " + path_);
    done += static_cast<size_t>(n);
  }
  return Status();
}

Status File::pwrite_all(ByteSpan data, uint64_t offset) {
  size_t done = 0;
  while (done < data.size()) {
    RETURN_IF_ERROR(faults::tick(faults::Op::kPWrite));
    size_t want = data.size() - done;
    if (faults::tick_short(faults::Op::kPWrite)) want = std::max<size_t>(1, want / 2);
    const ssize_t n = ::pwrite(fd_, data.data() + done, want, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::from_errno("pwrite " + path_);
    }
    if (n == 0) return Status::error(Err::kIo, "pwrite returned 0 on " + path_);
    done += static_cast<size_t>(n);
  }
  return Status();
}

Result<size_t> File::pread_upto(uint8_t* buf, size_t n, uint64_t offset) {
  size_t done = 0;
  while (done < n) {
    RETURN_IF_ERROR(faults::tick(faults::Op::kPRead));
    const ssize_t r = ::pread(fd_, buf + done, n - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return Status::from_errno("pread " + path_);
    }
    if (r == 0) break;  // EOF
    done += static_cast<size_t>(r);
  }
  return done;
}

Status File::pread_exact(uint8_t* buf, size_t n, uint64_t offset) {
  ASSIGN_OR_RETURN(const size_t got, pread_upto(buf, n, offset));
  if (got != n) {
    return Status::corrupt("short read on " + path_ + ": wanted " + std::to_string(n) +
                           " got " + std::to_string(got) + " at offset " +
                           std::to_string(offset));
  }
  return Status();
}

Status File::sync() {
  RETURN_IF_ERROR(faults::tick(faults::Op::kFsync));
  if (::fsync(fd_) != 0) return Status::from_errno("fsync " + path_);
  return Status();
}

Status File::truncate(uint64_t length) {
  RETURN_IF_ERROR(faults::tick(faults::Op::kTruncate));
  if (::ftruncate(fd_, static_cast<off_t>(length)) != 0) {
    return Status::from_errno("ftruncate " + path_);
  }
  return Status();
}

Result<uint64_t> File::size() const {
  struct stat st {};
  if (::fstat(fd_, &st) != 0) return Status::from_errno("fstat " + path_);
  return static_cast<uint64_t>(st.st_size);
}

Status File::close() {
  if (fd_ < 0) return Status();
  const int fd = fd_;
  fd_ = -1;
  // A close() error can report a deferred write failure, so it is checked like
  // any other syscall rather than fired and forgotten.
  if (::close(fd) != 0) return Status::from_errno("close " + path_);
  return Status();
}

// ---------------------------------------------------------------------------
// Directories and paths
// ---------------------------------------------------------------------------

Status fsync_dir(const std::string& path) {
  RETURN_IF_ERROR(faults::tick(faults::Op::kFsyncDir));
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return Status::from_errno("open dir " + path);
  const int rc = ::fsync(fd);
  const int saved = errno;
  ::close(fd);
  if (rc != 0) {
    errno = saved;
    return Status::from_errno("fsync dir " + path);
  }
  return Status();
}

Status make_dirs(const std::string& path) {
  std::string acc;
  size_t i = 0;
  if (!path.empty() && path[0] == '/') {
    acc = "/";
    i = 1;
  }
  while (i <= path.size()) {
    const size_t slash = path.find('/', i);
    const std::string part = path.substr(i, slash == std::string::npos ? std::string::npos
                                                                       : slash - i);
    if (!part.empty()) {
      if (!acc.empty() && acc.back() != '/') acc += '/';
      acc += part;
      if (::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) {
        return Status::from_errno("mkdir " + acc);
      }
    }
    if (slash == std::string::npos) break;
    i = slash + 1;
  }
  return Status();
}

Status rename_file(const std::string& from, const std::string& to) {
  RETURN_IF_ERROR(faults::tick(faults::Op::kRename));
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return Status::from_errno("rename " + from + " -> " + to);
  }
  return Status();
}

Status unlink_file(const std::string& path) {
  RETURN_IF_ERROR(faults::tick(faults::Op::kUnlink));
  if (::unlink(path.c_str()) != 0) {
    if (errno == ENOENT) return Status::error(Err::kNotFound, "unlink " + path, errno);
    return Status::from_errno("unlink " + path);
  }
  return Status();
}

Result<bool> path_exists(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0) return true;
  if (errno == ENOENT) return false;
  return Status::from_errno("stat " + path);
}

Result<uint64_t> file_size(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    if (errno == ENOENT) return Status::error(Err::kNotFound, "stat " + path, errno);
    return Status::from_errno("stat " + path);
  }
  return static_cast<uint64_t>(st.st_size);
}

Result<std::vector<std::string>> list_dir(const std::string& path) {
  DIR* d = ::opendir(path.c_str());
  if (d == nullptr) {
    if (errno == ENOENT) return Status::error(Err::kNotFound, "opendir " + path, errno);
    return Status::from_errno("opendir " + path);
  }
  std::vector<std::string> out;
  errno = 0;
  while (struct dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    out.push_back(name);
  }
  const int saved = errno;
  ::closedir(d);
  if (saved != 0) {
    errno = saved;
    return Status::from_errno("readdir " + path);
  }
  // Directory order is filesystem-defined; sorting makes every caller
  // deterministic, which matters for reproducible tests and for GC's scan order.
  std::sort(out.begin(), out.end());
  return out;
}

Status write_file_atomic(const std::string& path, ByteSpan data) {
  // The temp name must be unique per writer. A fixed "<path>.tmp" looks fine
  // single-threaded and is a data-corruption race the moment two threads publish
  // the same name: both open the same temp file, their writes interleave, and
  // both rename the result into place. Found by inspection before T6's stress
  // test could hit it (CHALLENGES B11).
  static std::atomic<uint64_t> seq{0};
  const std::string tmp = path + ".tmp." + std::to_string(::getpid()) + "." +
                          std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
  const size_t slash = path.rfind('/');
  const std::string dir = (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);

  {
    ASSIGN_OR_RETURN(File f, File::open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644));
    RETURN_IF_ERROR(f.write_all(data));
    // ORDER IS THE POINT (S4): the bytes must be durable BEFORE the name that
    // publishes them exists, or a crash can leave a valid-looking name pointing
    // at a file whose contents never reached the device.
    RETURN_IF_ERROR(f.sync());
    RETURN_IF_ERROR(f.close());
  }
  // rename() is atomic even when the destination exists, so a concurrent
  // publisher of the same name loses cleanly: one whole file wins, and no reader
  // ever observes a mixture.
  RETURN_IF_ERROR(rename_file(tmp, path));
  // And the rename itself is only durable once the directory is synced.
  RETURN_IF_ERROR(fsync_dir(dir));
  return Status();
}

Result<std::vector<uint8_t>> read_file(const std::string& path) {
  ASSIGN_OR_RETURN(File f, File::open(path, O_RDONLY));
  ASSIGN_OR_RETURN(const uint64_t n, f.size());
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  if (n > 0) RETURN_IF_ERROR(f.pread_exact(buf.data(), buf.size(), 0));
  return buf;
}

// ---------------------------------------------------------------------------
// FileLock
// ---------------------------------------------------------------------------

FileLock::~FileLock() = default;
FileLock::FileLock(FileLock&&) noexcept = default;
FileLock& FileLock::operator=(FileLock&&) noexcept = default;

Result<FileLock> FileLock::acquire(const std::string& path) {
  ASSIGN_OR_RETURN(File f, File::open(path, O_WRONLY | O_CREAT, 0644));
  if (::flock(f.fd(), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      return Status::error(Err::kLocked, "store already locked by another process: " + path,
                           errno);
    }
    return Status::from_errno("flock " + path);
  }
  FileLock l;
  l.file_ = std::move(f);
  return l;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

std::string path_join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (!a.empty() && a.back() == '/') return a + b;
  return a + "/" + b;
}

Status validate_component(const std::string& name) {
  if (name.empty()) return Status::invalid("empty path component");
  if (name == "." || name == "..") return Status::invalid("path component '" + name + "'");
  if (name.size() > 255) return Status::invalid("path component too long");
  if (name.find('/') != std::string::npos) {
    return Status::invalid("path component contains '/': " + name);
  }
  if (name.find('\0') != std::string::npos) {
    return Status::invalid("path component contains NUL");
  }
  return Status();
}

}  // namespace dedupe
