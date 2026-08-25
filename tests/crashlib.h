// Shared machinery for crash tests (SPEC S3): fork a child, kill it at a chosen
// syscall, then reopen the store in the parent and check what survived.
//
// WHY FORK RATHER THAN THREADS OR A FLAG.
//   A crash is not an error return. It skips destructors, buffered writes and
//   every cleanup path the code has, and a store must be consistent anyway. Only
//   a real process death exercises that, so the child calls _exit() from inside
//   the I/O layer at the chosen operation and the parent picks up the pieces.
//
// WHAT IS AND IS NOT PROVEN.
//   The child dies; the machine does not. Bytes already passed to write() are in
//   the kernel's page cache and survive. So this proves PROCESS-CRASH
//   CONSISTENCY, not power-loss durability -- SPEC 8.6 makes the same
//   distinction, and blurring the two is the standard way storage projects
//   overclaim.
#pragma once

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "dedupe/gc.h"
#include "dedupe/io.h"
#include "dedupe/store.h"

namespace crashtest {

// Deterministic content, identical in parent and child, so the parent knows
// exactly what every file should contain without communicating with the child.
inline std::vector<uint8_t> payload(int index, size_t n) {
  std::vector<uint8_t> v(n);
  std::mt19937_64 rng(0xC0FFEE00ull + static_cast<uint64_t>(index));
  for (size_t i = 0; i + 8 <= n; i += 8) {
    const uint64_t r = rng();
    std::memcpy(v.data() + i, &r, 8);
  }
  for (size_t i = (n / 8) * 8; i < n; i++) v[i] = static_cast<uint8_t>(rng() & 0xFF);
  return v;
}

inline std::string name_of(int index) { return "file" + std::to_string(index); }

// Runs `body` in a forked child and waits for it. Returns the child's exit
// status. The caller must NOT hold the store open across this: the child needs
// the flock.
template <class F>
int run_in_child(F&& body) {
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  if (pid == 0) {
    body();
    std::fflush(nullptr);
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  return status;
}

// The child's workload: ingest `count` files, optionally deleting some and
// running a GC cycle, with a crash armed at (op, nth).
struct ChildPlan {
  std::string store_path;
  int count = 6;
  size_t file_bytes = 300000;
  bool skip_ingest = false;  // for GC scenarios: operate on what is already there
  bool delete_some = false;
  bool run_gc = false;
  dedupe::faults::Op crash_op = dedupe::faults::Op::kWrite;
  uint64_t crash_at = 0;  // 0 = do not crash
};

inline void child_body(const ChildPlan& plan) {
  using namespace dedupe;
  auto opened = Store::open(plan.store_path);
  if (!opened.ok()) ::_exit(20);
  Store& store = *opened.value();

  // Arm AFTER opening, so the crash lands in the workload rather than in
  // recovery -- otherwise every scenario would test the same three syscalls.
  faults::reset();
  if (plan.crash_at != 0) faults::abort_at(plan.crash_op, plan.crash_at);

  if (!plan.skip_ingest) {
    auto session = store.begin_ingest();
    if (!session.ok()) ::_exit(21);
    for (int i = 0; i < plan.count; i++) {
      (void)session.value().ingest_bytes(name_of(i), payload(i, plan.file_bytes)).ok();
    }
    (void)session.value().finish();
  }
  if (plan.delete_some) {
    for (int i = 0; i < plan.count; i += 2) (void)store.remove(name_of(i));
  }
  if (plan.run_gc) {
    Gc gc(&store);
    (void)gc.run_once().ok();
  }
}

struct Outcome {
  bool opened = false;
  bool verified = false;
  int files_present = 0;
  int files_correct = 0;
  std::vector<std::string> problems;
};

// The parent's job: reopen, verify, and confirm that every file which EXISTS is
// byte-perfect. A file may legitimately be missing -- it may have been mid-ingest
// when the child died -- but a file that exists and is wrong is a corrupt store.
inline Outcome inspect(const std::string& store_path, int count, size_t file_bytes) {
  using namespace dedupe;
  Outcome out;
  faults::reset();  // the parent must never inherit an armed fault
  auto opened = Store::open(store_path);
  if (!opened.ok()) {
    out.problems.push_back("open failed: " + opened.status().to_string());
    return out;
  }
  out.opened = true;
  Store& store = *opened.value();

  auto rep = store.verify(VerifyLevel::kDeep);
  if (!rep.ok()) {
    out.problems.push_back("verify errored: " + rep.status().to_string());
    return out;
  }
  out.verified = rep.value().ok();
  for (const std::string& p : rep.value().problems) out.problems.push_back(p);

  for (int i = 0; i < count; i++) {
    auto exists = store.exists(name_of(i));
    if (!exists.ok() || !exists.value()) continue;
    out.files_present++;
    auto got = store.restore_bytes(name_of(i));
    if (!got.ok()) {
      out.problems.push_back(name_of(i) + ": restore failed: " + got.status().to_string());
      continue;
    }
    if (got.value() != payload(i, file_bytes)) {
      out.problems.push_back(name_of(i) + ": restored bytes differ");
      continue;
    }
    out.files_correct++;
  }
  return out;
}

}  // namespace crashtest
