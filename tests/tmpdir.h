// A scratch directory that cleans itself up. Test-only.
//
// mkdtemp rather than a fixed path: suites run in parallel under ctest -j, and
// two tests sharing /tmp/dedupe-test would produce failures that look like
// corruption bugs.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#include "dedupe/io.h"

namespace testing {

class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/dedupe-test-XXXXXX";
    const char* p = ::mkdtemp(tmpl);
    path_ = (p != nullptr) ? p : "/tmp/dedupe-test-fallback";
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);  // best effort; never throws
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string file(const std::string& name) const { return dedupe::path_join(path_, name); }

 private:
  std::string path_;
};

// Flip one byte in a file, in place. The cheapest way to simulate bit rot.
inline bool corrupt_byte(const std::string& path, uint64_t offset, uint8_t xor_mask = 0xFF) {
  FILE* f = ::fopen(path.c_str(), "r+b");
  if (f == nullptr) return false;
  if (::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) { ::fclose(f); return false; }
  int c = ::fgetc(f);
  if (c == EOF) { ::fclose(f); return false; }
  if (::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) { ::fclose(f); return false; }
  const int rc = ::fputc(c ^ xor_mask, f);
  ::fclose(f);
  return rc != EOF;
}

inline bool truncate_file(const std::string& path, uint64_t length) {
  std::error_code ec;
  std::filesystem::resize_file(path, length, ec);
  return !ec;
}

}  // namespace testing
