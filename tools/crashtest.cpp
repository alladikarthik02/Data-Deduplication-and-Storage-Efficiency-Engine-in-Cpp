// Exhaustive crash sweep (SPEC S3, §4.3).
//
// tests/test_recovery.cpp checks a curated set of injection points on every ctest
// run. This tool sweeps EVERY operation index in a range for every syscall class,
// which is far too slow for the gate but is what you run when you have changed
// the durability path and want to know it holds everywhere rather than at the
// fourteen points someone once thought were interesting.
//
//   ./scripts/dev.sh ./build-none/crashtest [max_n]
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../tests/crashlib.h"

using namespace dedupe;
using namespace crashtest;

namespace {

constexpr int kFiles = 5;
constexpr size_t kBytes = 200000;

struct Case {
  const char* label;
  faults::Op op;
  bool gc;
};

const Case kCases[] = {
    {"write", faults::Op::kWrite, false},   {"fsync", faults::Op::kFsync, false},
    {"rename", faults::Op::kRename, false}, {"fsync_dir", faults::Op::kFsyncDir, false},
    {"open", faults::Op::kOpen, false},     {"truncate", faults::Op::kTruncate, false},
    {"gc/write", faults::Op::kWrite, true}, {"gc/fsync", faults::Op::kFsync, true},
    {"gc/unlink", faults::Op::kUnlink, true},
};

// A cast to (void) does NOT silence GCC's warn_unused_result -- the compiler
// caught this immediately, which is the -Werror=unused-result rule from
// CMakeLists earning its place (SPEC S11). Check it properly instead.
bool rm_rf(const std::string& path) {
  return std::system(("rm -rf " + path).c_str()) == 0;
}

std::string make_scratch() {
  char tmpl[] = "/tmp/dedupe-crashsweep-XXXXXX";
  const char* p = ::mkdtemp(tmpl);
  return p != nullptr ? p : "/tmp/dedupe-crashsweep";
}

}  // namespace

int main(int argc, char** argv) {
  const uint64_t max_n = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 25;
  const std::string scratch = make_scratch();
  std::printf("dedupe crash sweep -- every injection point 1..%llu for %zu syscall classes\n\n",
              static_cast<unsigned long long>(max_n), sizeof(kCases) / sizeof(kCases[0]));

  uint64_t runs = 0, crashes = 0, failures = 0;
  for (const Case& c : kCases) {
    uint64_t case_crashes = 0, case_fail = 0;
    for (uint64_t n = 1; n <= max_n; n++) {
      const std::string sp = scratch + "/s" + std::to_string(runs);
      if (!rm_rf(sp)) return 1;

      if (c.gc) {  // the GC cases need a store that already holds data
        ChildPlan warm;
        warm.store_path = sp;
        warm.count = kFiles;
        warm.file_bytes = kBytes;
        (void)run_in_child([&] { child_body(warm); });
      }

      ChildPlan plan;
      plan.store_path = sp;
      plan.count = kFiles;
      plan.file_bytes = kBytes;
      plan.crash_op = c.op;
      plan.crash_at = n;
      plan.skip_ingest = c.gc;
      plan.delete_some = c.gc;
      plan.run_gc = c.gc;
      const int status = run_in_child([&] { child_body(plan); });
      const bool crashed = WIFEXITED(status) && WEXITSTATUS(status) == 9;
      if (crashed) { crashes++; case_crashes++; }

      const Outcome out = inspect(sp, kFiles, kBytes);
      // Presence implies correctness. A file may be missing (it was never
      // published); a file that exists and is wrong is data loss.
      const bool ok = out.opened && out.verified && out.files_correct == out.files_present;
      if (!ok) {
        failures++;
        case_fail++;
        std::printf("  FAIL %s @ %llu: opened=%d verified=%d %d/%d correct\n", c.label,
                    static_cast<unsigned long long>(n), out.opened ? 1 : 0,
                    out.verified ? 1 : 0, out.files_correct, out.files_present);
        for (const std::string& p : out.problems) std::printf("        %s\n", p.c_str());
      }
      if (!rm_rf(sp)) return 1;
      runs++;
    }
    std::printf("  %-12s %llu points, %llu real crashes, %llu failures\n", c.label,
                static_cast<unsigned long long>(max_n),
                static_cast<unsigned long long>(case_crashes),
                static_cast<unsigned long long>(case_fail));
  }
  if (!rm_rf(scratch)) return 1;
  std::printf("\n%llu runs, %llu real crashes, %llu failures -- %s\n",
              static_cast<unsigned long long>(runs), static_cast<unsigned long long>(crashes),
              static_cast<unsigned long long>(failures), failures == 0 ? "OK" : "FAILURES ABOVE");
  return failures == 0 ? 0 : 1;
}
