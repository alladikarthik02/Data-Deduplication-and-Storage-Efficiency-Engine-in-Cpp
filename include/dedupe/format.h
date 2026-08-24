// On-disk formats: container headers, chunk records, recipes.
//
// EVERY DECODER HERE IS A PARSER FOR UNTRUSTED INPUT (SPEC S9).
//   The bytes may have been truncated by a crash, corrupted by bit rot, or
//   crafted. The rule is absolute: a length read from disk is validated against
//   what the buffer actually contains BEFORE it is used to size, index, or
//   allocate anything. The classic failure is `entries.reserve(chunk_count)`
//   where chunk_count came off the disk -- one flipped byte becomes a request
//   for terabytes of RAM. There is a test for exactly that.
//
// LITTLE-ENDIAN, EXPLICITLY.
//   T0 asserts the host is little-endian, but every field is still read and
//   written byte by byte rather than by memcpy'ing an integer. It costs nothing
//   the compiler cannot optimize away, and it keeps the format a specification
//   rather than a memory dump. (Note the deliberate contrast with SHA-256, which
//   is big-endian internally -- mixing the two conventions silently is a classic
//   bug, so both are always spelled out.)
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "dedupe/status.h"
#include "dedupe/types.h"

namespace dedupe {

// --- little-endian primitives ----------------------------------------------

inline void put_u32le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
inline void put_u64le(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint32_t get_u32le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t get_u64le(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

// Hard ceiling on any chunk, enforced at every boundary. 17 bits is what the
// index entry's packed length field holds (SPEC 3.4); a longer chunk would be
// silently truncated there, which is corruption rather than a limitation.
inline constexpr uint32_t kMaxChunkLength = 131071;

// --- container file header (32 bytes) --------------------------------------
//
// A container is not just "a file of records": it carries the store's UUID so a
// container from a different store cannot be mistaken for one of ours after a
// misplaced copy, and an id so a scan can verify it is where the index thinks.
struct ContainerHeader {
  static constexpr uint32_t kMagic = 0x544E4344;  // "DCNT" little-endian
  static constexpr uint32_t kVersion = 1;
  static constexpr size_t kSize = 32;

  uint32_t version = kVersion;
  uint32_t container_id = 0;
  uint32_t flags = 0;
  std::array<uint8_t, 16> store_uuid{};

  void encode(uint8_t out[kSize]) const;
  static Result<ContainerHeader> decode(ByteSpan in);
};

// --- chunk record header (48 bytes) ----------------------------------------
//
// Self-describing on purpose (SPEC 3.3): a container alone is enough to rebuild
// the whole index, and a crash mid-append is detectable because the torn tail
// fails either its magic, its length bound, or its CRC.
struct ChunkHeader {
  static constexpr uint32_t kMagic = 0x4B4E4843;  // "CHNK" little-endian
  static constexpr size_t kSize = 48;

  uint32_t length = 0;
  uint32_t crc32c = 0;
  uint32_t flags = 0;
  Digest32 fp{};

  void encode(uint8_t out[kSize]) const;
  static Result<ChunkHeader> decode(ByteSpan in);
};

// --- recipe ----------------------------------------------------------------
//
// The per-file "how to rebuild me" list. Together with the containers it is the
// store's ground truth; the in-memory index is only a cache of what these say.

struct RecipeEntry {
  Digest32 fp{};
  uint32_t length = 0;
};

struct Recipe {
  static constexpr uint32_t kMagic = 0x45504352;  // "RCPE" little-endian
  static constexpr uint32_t kVersion = 1;
  static constexpr size_t kHeaderSize = 64;
  static constexpr size_t kEntrySize = 36;  // fp[32] + length u32

  std::string name;
  uint64_t logical_size = 0;
  Digest32 file_sha256{};  // whole-file digest: the end-to-end restore check (S1)
  std::vector<RecipeEntry> entries;

  std::vector<uint8_t> encode() const;
  static Result<Recipe> decode(ByteSpan in);

  uint64_t total_chunk_bytes() const {
    uint64_t n = 0;
    for (const auto& e : entries) n += e.length;
    return n;
  }
};

}  // namespace dedupe
