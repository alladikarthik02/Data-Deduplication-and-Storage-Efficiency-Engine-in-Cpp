// Shared vocabulary types.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dedupe {

// A chunk fingerprint: the full 256-bit SHA-256 of the chunk's bytes.
// Fixed-size std::array (not std::string/vector) on purpose -- it lives inside
// index entries and recipe records, where a heap pointer per fingerprint would
// wreck both the memory footprint (SPEC R3.1) and cache behaviour.
using Digest32 = std::array<uint8_t, 32>;

using ByteSpan = std::span<const uint8_t>;

// Convenience for tests and CLI output. Never used on the hot path.
inline std::string to_hex(ByteSpan bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(bytes.size() * 2);
  for (size_t i = 0; i < bytes.size(); i++) {
    out[2 * i] = kHex[bytes[i] >> 4];
    out[2 * i + 1] = kHex[bytes[i] & 0x0f];
  }
  return out;
}

inline std::string to_hex(const Digest32& d) { return to_hex(ByteSpan(d.data(), d.size())); }

// Returns false on any malformed input rather than throwing or asserting:
// this parses text that may come from a file or a user (SPEC S9).
inline bool from_hex(std::string_view s, Digest32& out) {
  if (s.size() != 64) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < 32; i++) {
    const int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

// Helper for turning any trivially-copyable buffer into bytes.
inline ByteSpan as_bytes(const void* p, size_t n) {
  return ByteSpan(static_cast<const uint8_t*>(p), n);
}
inline ByteSpan as_bytes(std::string_view s) {
  return ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace dedupe
