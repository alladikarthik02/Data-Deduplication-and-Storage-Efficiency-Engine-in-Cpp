// Crash recovery (SPEC S3, S4).
//
// The claim: after a kill -9 at ANY point, the store opens, `verify --deep` is
// clean, and every file that exists is byte-perfect. Files can go missing -- one
// that was mid-ingest when the process died was never published -- but a file
// that exists and is wrong means the engine lost data while reporting success.
//
// Crashes are injected at the syscall level and are deterministic: the child
// dies at the Nth write / fsync / rename / unlink, so a failure can be replayed
// exactly.
#include <cstdio>
#include <string>
#include <vector>

#include "crashlib.h"
#include "test.h"
#include "tmpdir.h"

using namespace dedupe;
using namespace crashtest;

namespace {

struct Point {
  const char* label;
  faults::Op op;
  uint64_t nth;
  bool delete_some;
  bool run_gc;
};

// Injection points chosen to land in different phases: mid-record, at a
// container sync, at the recipe publish, at the directory sync that makes the
// publish durable, and inside GC's compaction.
const Point kPoints[] = {
    {"1st write (mid container append)", faults::Op::kWrite, 1, false, false},
    {"3rd write", faults::Op::kWrite, 3, false, false},
    {"8th write", faults::Op::kWrite, 8, false, false},
    {"1st fsync (container sync)", faults::Op::kFsync, 1, false, false},
    {"4th fsync", faults::Op::kFsync, 4, false, false},
    {"1st rename (recipe publish)", faults::Op::kRename, 1, false, false},
    {"3rd rename", faults::Op::kRename, 3, false, false},
    {"1st dir fsync (publish durability)", faults::Op::kFsyncDir, 1, false, false},
    {"5th dir fsync", faults::Op::kFsyncDir, 5, false, false},
    {"1st open", faults::Op::kOpen, 1, false, false},
    {"during GC: 1st unlink (container retire)", faults::Op::kUnlink, 1, true, true},
    {"during GC: 2nd unlink", faults::Op::kUnlink, 2, true, true},
    {"during GC: 1st fsync (journal BEGIN)", faults::Op::kFsync, 1, true, true},
    {"during GC: 2nd fsync (compaction dst)", faults::Op::kFsync, 2, true, true},
    {"during GC: 3rd fsync (journal COMMIT)", faults::Op::kFsync, 3, true, true},
    {"during GC: 1st write (compaction copy)", faults::Op::kWrite, 1, true, true},
};

constexpr int kFiles = 6;
constexpr size_t kBytes = 300000;

}  // namespace

// Every injection point: crash, reopen, verify, restore everything present.
TEST(recovery_survives_a_crash_at_every_injection_point) {
  int checked = 0;
  for (const Point& pt : kPoints) {
    TCTX(pt.label);
    testing::TempDir dir;
    const std::string sp = dir.file("store");

    ChildPlan plan;
    plan.store_path = sp;
    plan.count = kFiles;
    plan.file_bytes = kBytes;
    plan.delete_some = pt.delete_some;
    plan.run_gc = pt.run_gc;
    // GC scenarios operate on a store a previous child already filled;
    // re-ingesting would dedup everything and the syscall counts would never
    // reach the injection point.
    plan.skip_ingest = pt.run_gc;
    plan.crash_op = pt.op;
    plan.crash_at = pt.nth;

    // For the GC scenarios the store must already hold data, so run a clean
    // child first and only then crash the second one.
    if (pt.run_gc) {
      ChildPlan warmup = plan;
      warmup.crash_at = 0;
      warmup.delete_some = false;
      warmup.run_gc = false;
      warmup.skip_ingest = false;  // the warmup is what puts data there
      const int st = run_in_child([&] { child_body(warmup); });
      REQUIRE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    const int status = run_in_child([&] { child_body(plan); });
    // The child either crashed where we told it to, or finished the workload
    // before reaching that many operations. Both are legitimate.
    const bool crashed = WIFEXITED(status) && WEXITSTATUS(status) == 9;
    const bool finished = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!crashed && !finished) {
      std::printf("  [warn] %s: unexpected child status %d\n", pt.label, status);
    }
    CHECK(crashed || finished);

    const Outcome out = inspect(sp, kFiles, kBytes);
    std::printf("  [info] %-38s crashed=%d  files present %d, all correct %d\n", pt.label,
                crashed ? 1 : 0, out.files_present, out.files_correct);
    for (const std::string& p : out.problems) std::printf("      PROBLEM: %s\n", p.c_str());

    CHECK(out.opened);
    CHECK(out.verified);
    // The load-bearing assertion: presence implies correctness.
    CHECK_EQ(out.files_correct, out.files_present);
    checked++;
  }
  CHECK_EQ(checked, static_cast<int>(sizeof(kPoints) / sizeof(kPoints[0])));
}

// A crash, then recovery, then MORE work, then another crash. Recovery must
// leave a store that is fully usable, not merely readable -- and the second
// recovery must cope with whatever the first one left behind.
TEST(recovery_is_repeatable_and_leaves_a_usable_store) {
  testing::TempDir dir;
  const std::string sp = dir.file("store");

  for (int round = 0; round < 4; round++) {
    TCTX("round=" << round);
    ChildPlan plan;
    plan.store_path = sp;
    plan.count = kFiles;
    plan.file_bytes = kBytes;
    plan.crash_op = (round % 2 == 0) ? faults::Op::kWrite : faults::Op::kFsync;
    plan.crash_at = static_cast<uint64_t>(2 + round * 3);
    plan.delete_some = (round == 2);
    plan.run_gc = (round == 2);
    (void)run_in_child([&] { child_body(plan); });

    const Outcome out = inspect(sp, kFiles, kBytes);
    CHECK(out.opened);
    CHECK(out.verified);
    CHECK_EQ(out.files_correct, out.files_present);
    for (const std::string& p : out.problems) std::printf("      PROBLEM: %s\n", p.c_str());
  }

  // After all that, a clean run must still complete and every file be perfect.
  ChildPlan clean;
  clean.store_path = sp;
  clean.count = kFiles;
  clean.file_bytes = kBytes;
  const int st = run_in_child([&] { child_body(clean); });
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  const Outcome out = inspect(sp, kFiles, kBytes);
  CHECK(out.opened);
  CHECK(out.verified);
  CHECK_EQ(out.files_present, kFiles);
  CHECK_EQ(out.files_correct, kFiles);
  std::printf("  [info] after 4 crash/recovery rounds, a clean run restored %d/%d files\n",
              out.files_correct, kFiles);
}

RUN_ALL()
