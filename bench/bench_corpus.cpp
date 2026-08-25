// T9: the résumé's storage-reduction number (R1.4).
//
// A dedup ratio is a property of the DATA, not the engine. Ten full backups of a
// slowly-changing tree dedup ~8x; ten tarballs of unrelated random bytes dedup
// 1.0x. Quoting a bare number is the most common way storage benchmarks lie, so
// this tool always prints the number together with the generator that produced
// it, and bounds it with controls on both sides:
//
//   workload   8 full backup generations of a ~2000-file tree, 3% of files
//              modified between generations, 1% created, 1% deleted
//   control A  the same sizes and count, but every generation entirely UNIQUE
//              random data -- the floor; anything above ~1.0x here would mean
//              the engine is finding duplication that does not exist
//   control B  a single generation -- what one backup alone dedups
//   sweep      churn rate 1% / 3% / 10%, so the headline is a point on a curve
//              rather than a cherry-picked configuration
//
// Chunk payloads are deliberately NOT compressed (SPEC 1), so every byte of the
// reduction is attributable to deduplication.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "dedupe/store.h"

using namespace dedupe;
using Clock = std::chrono::steady_clock;

namespace {

struct SourceFile {
  std::string name;
  std::vector<uint8_t> content;
};

void fill_random(std::vector<uint8_t>& v, std::mt19937_64& rng) {
  size_t i = 0;
  for (; i + 8 <= v.size(); i += 8) {
    const uint64_t r = rng();
    std::memcpy(v.data() + i, &r, 8);
  }
  for (; i < v.size(); i++) v[i] = static_cast<uint8_t>(rng() & 0xFF);
}

// A realistic-ish size mix: mostly small files, a long tail of large ones. It
// matters because a file below the 2 KiB minimum chunk size is a single chunk
// and only dedups if it is bit-identical -- an all-large-files corpus would
// flatter the ratio.
size_t pick_size(std::mt19937_64& rng) {
  const uint64_t r = rng() % 100;
  if (r < 60) return 1024 + rng() % (16 * 1024);         // 60%: 1-17 KiB
  if (r < 90) return 16 * 1024 + rng() % (240 * 1024);   // 30%: 16-256 KiB
  if (r < 99) return 256 * 1024 + rng() % (1u << 21);    //  9%: 0.25-2.3 MiB
  return (2u << 20) + rng() % (6u << 20);                //  1%: 2-8 MiB
}

std::vector<SourceFile> make_generation_one(std::mt19937_64& rng, int count) {
  std::vector<SourceFile> files;
  files.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) {
    SourceFile f;
    f.name = "tree/dir" + std::to_string(i % 64) + "/file" + std::to_string(i) + ".dat";
    f.content.resize(pick_size(rng));
    fill_random(f.content, rng);
    files.push_back(std::move(f));
  }
  return files;
}

// One backup generation's worth of change: a few files edited in place, appended
// to or truncated, a few created, a few deleted.
void mutate(std::vector<SourceFile>& files, std::mt19937_64& rng, double churn, int gen,
            int* modified, int* created, int* deleted) {
  const size_t n = files.size();
  const size_t to_modify = static_cast<size_t>(static_cast<double>(n) * churn);
  for (size_t k = 0; k < to_modify; k++) {
    SourceFile& f = files[rng() % files.size()];
    switch (rng() % 3) {
      case 0: {  // in-place edit somewhere in the middle
        if (f.content.size() < 4096) break;
        const size_t at = rng() % (f.content.size() - 2048);
        const size_t len = 512 + rng() % 1536;
        for (size_t i = at; i < std::min(at + len, f.content.size()); i++) {
          f.content[i] = static_cast<uint8_t>(rng() & 0xFF);
        }
        break;
      }
      case 1: {  // append
        const size_t add = 1024 + rng() % (64 * 1024);
        const size_t old = f.content.size();
        f.content.resize(old + add);
        for (size_t i = old; i < f.content.size(); i++) {
          f.content[i] = static_cast<uint8_t>(rng() & 0xFF);
        }
        break;
      }
      default: {  // truncate
        if (f.content.size() < 8192) break;
        f.content.resize(f.content.size() - (1024 + rng() % 4096));
        break;
      }
    }
    (*modified)++;
  }
  const size_t churn_files = std::max<size_t>(1, n / 100);
  for (size_t k = 0; k < churn_files; k++) {  // created
    SourceFile f;
    f.name = "tree/new/gen" + std::to_string(gen) + "-" + std::to_string(k) + ".dat";
    f.content.resize(pick_size(rng));
    fill_random(f.content, rng);
    files.push_back(std::move(f));
    (*created)++;
  }
  for (size_t k = 0; k < churn_files && files.size() > 10; k++) {  // deleted
    files.erase(files.begin() + static_cast<long>(rng() % files.size()));
    (*deleted)++;
  }
}

bool rm_rf(const std::string& p) { return std::system(("rm -rf " + p).c_str()) == 0; }

struct RunResult {
  double ratio = 0;
  uint64_t logical = 0;
  uint64_t physical = 0;
  uint64_t unique_chunks = 0;
  uint64_t files = 0;
  size_t index_bytes = 0;
  double seconds = 0;
};

// Ingest `generations` full backups of `files`, mutating between them, and report
// the cumulative ratio after each.
RunResult run_workload(const std::string& sp, int count, int generations, double churn,
                       uint64_t seed, bool unique_every_generation, bool verbose) {
  RunResult res;
  if (!rm_rf(sp)) return res;
  auto opened = Store::open(sp);
  if (!opened.ok()) {
    std::fprintf(stderr, "open: %s\n", opened.status().to_string().c_str());
    return res;
  }
  Store& store = *opened.value();

  std::mt19937_64 rng(seed);
  std::vector<SourceFile> files = make_generation_one(rng, count);

  const auto t0 = Clock::now();
  for (int gen = 1; gen <= generations; gen++) {
    int modified = 0, created = 0, deleted = 0;
    if (gen > 1) {
      if (unique_every_generation) {
        // The control: replace every byte, so nothing can dedup against anything.
        for (SourceFile& f : files) fill_random(f.content, rng);
      } else {
        mutate(files, rng, churn, gen, &modified, &created, &deleted);
      }
    }
    auto session = store.begin_ingest();
    if (!session.ok()) return res;
    for (const SourceFile& f : files) {
      const std::string name = "gen" + std::to_string(gen) + "/" + f.name;
      if (!session.value().ingest_bytes(name, f.content).ok()) return res;
    }
    if (!session.value().finish().ok()) return res;

    auto st = store.stats();
    if (!st.ok()) return res;
    res.logical = st.value().logical_bytes;
    res.physical = st.value().physical_bytes;
    res.ratio = st.value().dedup_ratio();
    res.unique_chunks = st.value().unique_chunks;
    res.files = st.value().files;
    res.index_bytes = st.value().index_bytes;
    if (verbose) {
      std::printf("    gen %d: %5zu files (%d modified, %d created, %d deleted)  "
                  "logical %7.1f MiB  physical %7.1f MiB  ratio %5.2fx\n",
                  gen, files.size(), modified, created, deleted,
                  static_cast<double>(res.logical) / 1048576.0,
                  static_cast<double>(res.physical) / 1048576.0, res.ratio);
    }
  }
  res.seconds = std::chrono::duration<double>(Clock::now() - t0).count();

  // Correctness is not optional just because this is a benchmark: a ratio from a
  // store that cannot restore its own data is meaningless.
  auto rep = store.verify(VerifyLevel::kDeep);
  if (!rep.ok() || !rep.value().ok()) {
    std::fprintf(stderr, "  !! verify FAILED -- ratio above is meaningless\n");
    if (rep.ok()) {
      for (const std::string& p : rep.value().problems) std::fprintf(stderr, "     %s\n", p.c_str());
    }
    res.ratio = -1;
  }
  opened.value().reset();
  if (!rm_rf(sp)) return res;
  return res;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string base = (argc > 1) ? argv[1] : "/tmp/dedupe-bench-corpus";
  const int kFiles = 2000;
  const int kGenerations = 8;

  std::printf("dedupe T9 -- storage reduction on a defined backup workload\n\n");
  std::printf("  corpus: %d files, size mix 60%% 1-17 KiB / 30%% 16-256 KiB / 9%% 0.25-2.3 MiB"
              " / 1%% 2-8 MiB\n", kFiles);
  std::printf("  chunking: FastCDC 2K/8K/64K, SHA-256 fingerprints, NO compression\n\n");

  std::printf("  WORKLOAD: %d full backup generations, 3%% of files modified between each\n",
              kGenerations);
  const RunResult main_run =
      run_workload(base + "-main", kFiles, kGenerations, 0.03, 12345, false, true);
  std::printf("\n  => %d generations: %.1f MiB logical, %.1f MiB physical, "
              "**%.2fx storage reduction**\n",
              kGenerations, static_cast<double>(main_run.logical) / 1048576.0,
              static_cast<double>(main_run.physical) / 1048576.0, main_run.ratio);
  std::printf("     %llu files, %llu unique chunks, index %.1f MiB (%.1f B/chunk, %.3f%% of "
              "logical)\n",
              static_cast<unsigned long long>(main_run.files),
              static_cast<unsigned long long>(main_run.unique_chunks),
              static_cast<double>(main_run.index_bytes) / 1048576.0,
              main_run.unique_chunks
                  ? static_cast<double>(main_run.index_bytes) /
                        static_cast<double>(main_run.unique_chunks)
                  : 0.0,
              main_run.logical ? 100.0 * static_cast<double>(main_run.index_bytes) /
                                     static_cast<double>(main_run.logical)
                               : 0.0);
  std::printf("     %.1fs total, %.1f MB/s end to end (%llu file ingests, each with its own "
              "durable publish)\n\n",
              main_run.seconds,
              (static_cast<double>(main_run.logical) / 1048576.0) / main_run.seconds,
              static_cast<unsigned long long>(main_run.files));
  if (argc > 2 && std::string(argv[2]) == "main") return 0;

  std::printf("  CONTROL A: every generation entirely unique random data (the floor)\n");
  const RunResult control_a =
      run_workload(base + "-uniq", kFiles / 4, kGenerations, 0.0, 999, true, false);
  std::printf("    => %.2fx  (expected ~1.0x; anything higher means we are finding "
              "duplication that is not there)\n\n", control_a.ratio);

  std::printf("  CONTROL B: a single generation, no repeat backups\n");
  const RunResult control_b = run_workload(base + "-one", kFiles, 1, 0.03, 12345, false, false);
  std::printf("    => %.2fx  (what one backup dedups against itself)\n\n", control_b.ratio);

  std::printf("  SENSITIVITY: how the ratio moves with churn rate (%d generations)\n",
              kGenerations);
  std::printf("  %10s %14s %14s %10s\n", "churn/gen", "logical MiB", "physical MiB", "ratio");
  for (double churn : {0.01, 0.03, 0.10, 0.25}) {
    const RunResult r = run_workload(base + "-churn", kFiles / 2, kGenerations, churn, 777,
                                     false, false);
    std::printf("  %9.0f%% %14.1f %14.1f %9.2fx\n", churn * 100,
                static_cast<double>(r.logical) / 1048576.0,
                static_cast<double>(r.physical) / 1048576.0, r.ratio);
  }

  std::printf("\n  GENERATION CURVE at 3%% churn (how the ratio builds up)\n");
  std::printf("  %12s %10s\n", "generations", "ratio");
  for (int g : {1, 2, 4, 6, 8, 12}) {
    const RunResult r = run_workload(base + "-curve", kFiles / 2, g, 0.03, 4242, false, false);
    std::printf("  %12d %9.2fx\n", g, r.ratio);
  }
  return 0;
}
