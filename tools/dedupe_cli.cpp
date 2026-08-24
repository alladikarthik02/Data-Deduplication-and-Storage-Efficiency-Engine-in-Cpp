// dedupe -- command line interface.
//
// Deliberately thin: every operation is one call into Store, so the CLI cannot
// be where the interesting behaviour lives. Exit codes matter (scripts and the
// test harness depend on them): 0 success, 1 usage, 2 operation failed.
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dedupe/store.h"

using namespace dedupe;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: dedupe --store <dir> <command> [args]\n"
               "\n"
               "  ingest <file> [name]     add a file (name defaults to the path)\n"
               "  restore <name> <out>     rebuild a file\n"
               "  cat <name>               rebuild a file to stdout\n"
               "  rm <name>                delete a file (space returns via gc)\n"
               "  ls                       list stored files\n"
               "  stats                    dedup ratio, footprint, index cost\n"
               "  verify [--deep]          consistency check\n"
               "\n"
               "options:\n"
               "  --no-verify-fp           skip full-fingerprint checks on read\n");
  return 1;
}

std::string human(uint64_t bytes) {
  static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    u++;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f %s", v, kUnits[u]);
  return buf;
}

int fail(const Status& st) {
  std::fprintf(stderr, "error: %s\n", st.to_string().c_str());
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string store_path;
  StoreOptions opts;
  std::vector<std::string> args;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--store" && i + 1 < argc) {
      store_path = argv[++i];
    } else if (a == "--no-verify-fp") {
      opts.verify_fingerprint_on_read = false;
    } else {
      args.push_back(a);
    }
  }
  if (store_path.empty() || args.empty()) return usage();

  auto opened = Store::open(store_path, opts);
  if (!opened.ok()) return fail(opened.status());
  Store& store = *opened.value();
  const std::string cmd = args[0];

  if (cmd == "ingest") {
    if (args.size() < 2) return usage();
    const std::string path = args[1];
    const std::string name = args.size() > 2 ? args[2] : path;
    auto r = store.ingest_file(name, path);
    if (!r.ok()) return fail(r.status());
    const IngestStats& s = r.value();
    char ratio[32];
    // physical == 0 means every chunk already existed. "0.00x" would read as a
    // failure; it is the best possible outcome.
    if (s.physical_bytes == 0) std::snprintf(ratio, sizeof(ratio), "fully deduplicated");
    else std::snprintf(ratio, sizeof(ratio), "ratio %.2fx", s.dedup_ratio());
    std::printf("%s: %s logical, %s written, %" PRIu64 " chunks (%" PRIu64 " new, %" PRIu64
                " deduped), %s\n",
                name.c_str(), human(s.logical_bytes).c_str(), human(s.physical_bytes).c_str(),
                s.chunks_total, s.chunks_new, s.chunks_deduped, ratio);
    return 0;
  }
  if (cmd == "restore") {
    if (args.size() < 3) return usage();
    const Status st = store.restore(args[1], args[2]);
    if (!st.ok()) return fail(st);
    std::printf("restored %s -> %s\n", args[1].c_str(), args[2].c_str());
    return 0;
  }
  if (cmd == "cat") {
    if (args.size() < 2) return usage();
    auto r = store.restore_bytes(args[1]);
    if (!r.ok()) return fail(r.status());
    std::fwrite(r.value().data(), 1, r.value().size(), stdout);
    return 0;
  }
  if (cmd == "rm") {
    if (args.size() < 2) return usage();
    const Status st = store.remove(args[1]);
    if (!st.ok()) return fail(st);
    std::printf("removed %s (space returns after gc)\n", args[1].c_str());
    return 0;
  }
  if (cmd == "ls") {
    auto r = store.list();
    if (!r.ok()) return fail(r.status());
    for (const FileInfo& f : r.value()) {
      std::printf("%12s  %8" PRIu64 " chunks  %s\n", human(f.logical_size).c_str(), f.chunk_count,
                  f.name.c_str());
    }
    return 0;
  }
  if (cmd == "stats") {
    auto r = store.stats();
    if (!r.ok()) return fail(r.status());
    const StoreStats& s = r.value();
    std::printf("files             %" PRIu64 "\n", s.files);
    std::printf("logical           %s\n", human(s.logical_bytes).c_str());
    std::printf("physical          %s  (%" PRIu64 " containers)\n",
                human(s.physical_bytes).c_str(), s.containers);
    std::printf("dedup ratio       %.2fx\n", s.dedup_ratio());
    std::printf("unique chunks     %" PRIu64 "\n", s.unique_chunks);
    std::printf("chunk references  %" PRIu64 "\n", s.chunk_references);
    std::printf("index memory      %s  (%.1f B/chunk)\n", human(s.index_bytes).c_str(),
                s.unique_chunks ? static_cast<double>(s.index_bytes) /
                                      static_cast<double>(s.unique_chunks)
                                : 0.0);
    return 0;
  }
  if (cmd == "verify") {
    const bool deep = args.size() > 1 && args[1] == "--deep";
    auto r = store.verify(deep ? VerifyLevel::kDeep : VerifyLevel::kQuick);
    if (!r.ok()) return fail(r.status());
    const VerifyReport& rep = r.value();
    std::printf("checked %" PRIu64 " files, %" PRIu64 " chunks, %s\n", rep.files_checked,
                rep.chunks_checked, human(rep.bytes_checked).c_str());
    for (const std::string& p : rep.problems) std::printf("  PROBLEM: %s\n", p.c_str());
    if (!rep.ok()) {
      std::printf("%zu problem(s)\n", rep.problems.size());
      return 2;
    }
    std::printf("OK\n");
    return 0;
  }
  return usage();
}
