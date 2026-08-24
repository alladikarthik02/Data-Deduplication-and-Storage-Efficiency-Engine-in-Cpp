// The checked I/O layer (SPEC S4, S11, S12, S14).
//
// The point of this suite is that the ERROR paths are exercised, not just the
// happy path. An I/O wrapper whose failure handling has never run is a wrapper
// whose failure handling does not work.
#include <fcntl.h>

#include <string>
#include <vector>

#include "dedupe/io.h"
#include "dedupe/status.h"
#include "test.h"
#include "tmpdir.h"

using namespace dedupe;

namespace {
std::vector<uint8_t> bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}
struct FaultGuard {  // always disarm, even if a CHECK bails out early
  ~FaultGuard() { faults::reset(); }
};
}  // namespace

TEST(io_write_read_round_trip) {
  testing::TempDir dir;
  const std::string p = dir.file("a.bin");
  const auto data = bytes("hello, deduplicated world");

  {
    auto f = File::open(p, O_WRONLY | O_CREAT | O_TRUNC);
    REQUIRE(f.ok());
    CHECK(f.value().write_all(data).ok());
    CHECK(f.value().sync().ok());
    CHECK(f.value().close().ok());
  }
  auto got = read_file(p);
  REQUIRE(got.ok());
  CHECK(got.value() == data);
}

TEST(io_errors_are_distinguishable) {
  testing::TempDir dir;
  // Missing file must be kNotFound, not a generic I/O error: the store uses that
  // distinction to tell "first run" from "broken disk".
  auto missing = File::open(dir.file("nope"), O_RDONLY);
  CHECK(!missing.ok());
  CHECK(missing.status().code() == Err::kNotFound);

  const std::string p = dir.file("x");
  { auto f = File::open(p, O_WRONLY | O_CREAT); REQUIRE(f.ok()); }
  auto excl = File::open(p, O_WRONLY | O_CREAT | O_EXCL);
  CHECK(!excl.ok());
  CHECK(excl.status().code() == Err::kExists);
}

TEST(io_pread_exact_rejects_short_read) {
  testing::TempDir dir;
  const std::string p = dir.file("short");
  CHECK(write_file_atomic(p, bytes("1234")).ok());
  auto f = File::open(p, O_RDONLY);
  REQUIRE(f.ok());
  uint8_t buf[16];
  // Asking for more than exists must be an error, not a silently-partial buffer.
  const Status st = f.value().pread_exact(buf, sizeof(buf), 0);
  CHECK(!st.ok());
  CHECK(st.code() == Err::kCorrupt);
}

// THE test for this layer. write() is allowed to transfer fewer bytes than asked
// for; a wrapper without a retry loop silently truncates the record and reports
// success. Inject a short write and prove every byte still lands.
TEST(io_write_all_survives_short_writes) {
  FaultGuard guard;
  testing::TempDir dir;
  const std::string p = dir.file("shortw.bin");
  std::vector<uint8_t> data(10000);
  for (size_t i = 0; i < data.size(); i++) data[i] = static_cast<uint8_t>(i * 31);

  faults::reset();
  faults::short_nth(faults::Op::kWrite, 1);  // first write transfers only half
  {
    auto f = File::open(p, O_WRONLY | O_CREAT | O_TRUNC);
    REQUIRE(f.ok());
    CHECK(f.value().write_all(data).ok());
    CHECK(f.value().close().ok());
  }
  // Read the counter BEFORE disarming: reset() zeroes it, so asserting
  // afterwards was asserting 0 > 0. (My bug, not the code's -- CHALLENGES B8.)
  const uint64_t writes = faults::count(faults::Op::kWrite);
  faults::reset();

  auto got = read_file(p);
  REQUIRE(got.ok());
  CHECK_EQ(got.value().size(), data.size());
  CHECK(got.value() == data);
  // Prove the injection actually happened -- a test that silently stops
  // injecting is a test that stopped testing. One short write plus one retry
  // means write_all issued at least two writes for a single logical record.
  CHECK_GE(writes, uint64_t{2});
}

TEST(io_injected_failures_propagate) {
  FaultGuard guard;
  testing::TempDir dir;
  const std::string p = dir.file("f.bin");

  for (auto op : {faults::Op::kWrite, faults::Op::kFsync}) {
    TCTX(faults::op_name(op));
    faults::reset();
    faults::fail_nth(op, 1);
    auto f = File::open(p, O_WRONLY | O_CREAT | O_TRUNC);
    REQUIRE(f.ok());
    Status st = f.value().write_all(bytes("payload"));
    if (st.ok()) st = f.value().sync();
    CHECK(!st.ok());
    CHECK(st.code() == Err::kIo);
  }
  faults::reset();
}

// S4 -- the ordering that makes a publish durable. write_file_atomic must fsync
// the data BEFORE the rename that names it. Proven by failing the fsync and
// showing the rename never happened: if the order were reversed, the rename
// count would be 1.
TEST(io_atomic_write_syncs_before_it_renames) {
  FaultGuard guard;
  testing::TempDir dir;
  const std::string p = dir.file("published");

  faults::reset();
  faults::fail_nth(faults::Op::kFsync, 1);
  const Status st = write_file_atomic(p, bytes("new contents"));
  CHECK(!st.ok());
  CHECK_EQ(faults::count(faults::Op::kRename), uint64_t{0});
  faults::reset();

  // And the publish is all-or-nothing: a failed rename must leave any previous
  // contents intact rather than half-updated.
  CHECK(write_file_atomic(p, bytes("v1")).ok());
  faults::reset();
  faults::fail_nth(faults::Op::kRename, 1);
  CHECK(!write_file_atomic(p, bytes("v2-should-not-appear")).ok());
  faults::reset();
  auto got = read_file(p);
  REQUIRE(got.ok());
  CHECK(got.value() == bytes("v1"));
}

TEST(io_atomic_write_leaves_no_temp_file_on_success) {
  testing::TempDir dir;
  CHECK(write_file_atomic(dir.file("ok"), bytes("data")).ok());
  auto listing = list_dir(dir.path());
  REQUIRE(listing.ok());
  CHECK_EQ(listing.value().size(), size_t{1});
  CHECK_EQ(listing.value()[0], std::string("ok"));
}

// S14 -- two processes must never write the same store. flock is per-open-file,
// so a second acquire in the same process is the closest in-process proxy; the
// real cross-process case is covered by the crash tests in T8.
TEST(io_file_lock_is_exclusive) {
  testing::TempDir dir;
  const std::string p = dir.file("LOCK");
  auto a = FileLock::acquire(p);
  REQUIRE(a.ok());
  CHECK(a.value().held());
  auto b = FileLock::acquire(p);
  CHECK(!b.ok());
  CHECK(b.status().code() == Err::kLocked);
  // Released on destruction, so the next opener succeeds.
  { auto tmp = std::move(a.value()); }
  auto c = FileLock::acquire(p);
  CHECK(c.ok());
}

// S12 -- a name that reaches the filesystem must not escape the store directory.
TEST(io_validate_component_rejects_escapes) {
  CHECK(validate_component("file.txt").ok());
  CHECK(validate_component("a-b_c.1").ok());
  const std::vector<std::string> bad_names{"",     ".",     "..",
                                           "a/b",  "/abs",  "../../etc/passwd",
                                           std::string(256, 'x')};
  for (const std::string& bad : bad_names) {
    TCTX("name='" << bad << "'");
    const Status st = validate_component(bad);
    CHECK(!st.ok());
    CHECK(st.code() == Err::kInvalid);
  }
  std::string with_nul = "ab";
  with_nul.push_back('\0');
  with_nul += "cd";
  CHECK(!validate_component(with_nul).ok());
}

TEST(io_make_dirs_and_list_dir) {
  testing::TempDir dir;
  CHECK(make_dirs(dir.file("a/b/c")).ok());
  CHECK(make_dirs(dir.file("a/b/c")).ok());  // idempotent
  auto ex = path_exists(dir.file("a/b/c"));
  REQUIRE(ex.ok());
  CHECK(ex.value());

  // Deliberately NOT "a" -- that name is already the directory created above,
  // and renaming a file onto an existing directory fails with EISDIR.
  for (const char* n : {"z", "m", "b"}) CHECK(write_file_atomic(dir.file(n), bytes("x")).ok());
  auto listing = list_dir(dir.path());
  REQUIRE(listing.ok());
  CHECK_EQ(listing.value().size(), size_t{4});  // the "a" directory + three files
  // Sorted, so every caller (and every test) is deterministic. GC's scan order
  // depends on this.
  CHECK(std::is_sorted(listing.value().begin(), listing.value().end()));
  CHECK_EQ(listing.value()[0], std::string("a"));
}

TEST(io_file_is_move_only_and_closes_once) {
  testing::TempDir dir;
  auto a = File::open(dir.file("m"), O_WRONLY | O_CREAT);
  REQUIRE(a.ok());
  File moved = std::move(a.value());
  CHECK(moved.is_open());
  CHECK(!a.value().is_open());  // moved-from must not still own the fd
  CHECK(moved.write_all(bytes("hi")).ok());
  CHECK(moved.close().ok());
  CHECK(moved.close().ok());  // second close is a no-op, not a double-close
}

RUN_ALL()
