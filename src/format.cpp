#include "dedupe/format.h"

#include "dedupe/crc32c.h"

namespace dedupe {

// ---------------------------------------------------------------------------
// ContainerHeader
// ---------------------------------------------------------------------------

void ContainerHeader::encode(uint8_t out[kSize]) const {
  std::memset(out, 0, kSize);
  put_u32le(out + 0, kMagic);
  put_u32le(out + 4, version);
  put_u32le(out + 8, container_id);
  put_u32le(out + 12, flags);
  std::memcpy(out + 16, store_uuid.data(), 16);
}

Result<ContainerHeader> ContainerHeader::decode(ByteSpan in) {
  if (in.size() < kSize) {
    return Status::corrupt("container header: need " + std::to_string(kSize) + " bytes, have " +
                           std::to_string(in.size()));
  }
  const uint32_t magic = get_u32le(in.data());
  if (magic != kMagic) {
    return Status::corrupt("container header: bad magic 0x" + std::to_string(magic));
  }
  ContainerHeader h;
  h.version = get_u32le(in.data() + 4);
  // Refuse a version we do not understand rather than guessing at its layout.
  if (h.version != kVersion) {
    return Status::corrupt("container header: unsupported version " + std::to_string(h.version));
  }
  h.container_id = get_u32le(in.data() + 8);
  h.flags = get_u32le(in.data() + 12);
  std::memcpy(h.store_uuid.data(), in.data() + 16, 16);
  return h;
}

// ---------------------------------------------------------------------------
// ChunkHeader
// ---------------------------------------------------------------------------

void ChunkHeader::encode(uint8_t out[kSize]) const {
  put_u32le(out + 0, kMagic);
  put_u32le(out + 4, length);
  put_u32le(out + 8, crc32c);
  put_u32le(out + 12, flags);
  std::memcpy(out + 16, fp.data(), 32);
}

Result<ChunkHeader> ChunkHeader::decode(ByteSpan in) {
  if (in.size() < kSize) {
    return Status::corrupt("chunk header: need " + std::to_string(kSize) + " bytes, have " +
                           std::to_string(in.size()));
  }
  if (get_u32le(in.data()) != kMagic) return Status::corrupt("chunk header: bad magic");

  ChunkHeader h;
  h.length = get_u32le(in.data() + 4);
  // Bound the length BEFORE anyone uses it to size a read or a buffer. A
  // corrupted length field is the single most dangerous value in this format.
  if (h.length == 0) return Status::corrupt("chunk header: zero length");
  if (h.length > kMaxChunkLength) {
    return Status::corrupt("chunk header: length " + std::to_string(h.length) +
                           " exceeds max " + std::to_string(kMaxChunkLength));
  }
  h.crc32c = get_u32le(in.data() + 8);
  h.flags = get_u32le(in.data() + 12);
  std::memcpy(h.fp.data(), in.data() + 16, 32);
  return h;
}

// ---------------------------------------------------------------------------
// Recipe
// ---------------------------------------------------------------------------
//
// Layout:
//   0   4   magic "RCPE"
//   4   4   version
//   8   8   logical_size
//   16  8   chunk_count
//   24  32  file_sha256
//   56  4   name_len
//   60  4   body_crc32c    (CRC of everything after this 64-byte header)
//   64  ..  name bytes, then chunk_count * { fp[32], length u32 }

std::vector<uint8_t> Recipe::encode() const {
  const size_t body_size = name.size() + entries.size() * kEntrySize;
  std::vector<uint8_t> out(kHeaderSize + body_size, 0);

  uint8_t* body = out.data() + kHeaderSize;
  std::memcpy(body, name.data(), name.size());
  uint8_t* p = body + name.size();
  for (const auto& e : entries) {
    std::memcpy(p, e.fp.data(), 32);
    put_u32le(p + 32, e.length);
    p += kEntrySize;
  }

  put_u32le(out.data() + 0, kMagic);
  put_u32le(out.data() + 4, kVersion);
  put_u64le(out.data() + 8, logical_size);
  put_u64le(out.data() + 16, static_cast<uint64_t>(entries.size()));
  std::memcpy(out.data() + 24, file_sha256.data(), 32);
  put_u32le(out.data() + 56, static_cast<uint32_t>(name.size()));

  // The CRC covers the HEADER as well as the body -- everything except the four
  // bytes of the CRC field itself. It originally covered only the body, which
  // left file_sha256 (bytes 24..55) unprotected: a bit flip there would silently
  // change the digest that restore validates against, turning good data into a
  // reported corruption. `recipe_crc_catches_every_single_byte_flip` found it by
  // flipping all 252 bytes and catching only 220. See CHALLENGES B7.
  uint32_t c = crc32c(ByteSpan(out.data(), 60));         // header, up to the CRC field
  c = crc32c(ByteSpan(body, body_size), c);              // body, skipping the field
  put_u32le(out.data() + 60, c);
  return out;
}

Result<Recipe> Recipe::decode(ByteSpan in) {
  if (in.size() < kHeaderSize) {
    return Status::corrupt("recipe: truncated header (" + std::to_string(in.size()) + " bytes)");
  }
  if (get_u32le(in.data()) != kMagic) return Status::corrupt("recipe: bad magic");
  const uint32_t version = get_u32le(in.data() + 4);
  if (version != kVersion) {
    return Status::corrupt("recipe: unsupported version " + std::to_string(version));
  }

  const uint64_t logical_size = get_u64le(in.data() + 8);
  const uint64_t chunk_count = get_u64le(in.data() + 16);
  const uint32_t name_len = get_u32le(in.data() + 56);
  const uint32_t want_crc = get_u32le(in.data() + 60);

  // THE bounds check. chunk_count and name_len both come off the disk, and both
  // feed allocations. Validate them against the bytes we actually hold, using
  // arithmetic that cannot overflow: divide the remaining space rather than
  // multiplying chunk_count by the entry size.
  const size_t body_avail = in.size() - kHeaderSize;
  if (name_len > body_avail) {
    return Status::corrupt("recipe: name_len " + std::to_string(name_len) + " exceeds body " +
                           std::to_string(body_avail));
  }
  const size_t entries_avail = body_avail - name_len;
  if (chunk_count > entries_avail / kEntrySize) {
    return Status::corrupt("recipe: chunk_count " + std::to_string(chunk_count) +
                           " exceeds available " + std::to_string(entries_avail / kEntrySize));
  }
  const size_t body_size = name_len + static_cast<size_t>(chunk_count) * kEntrySize;
  if (body_size != body_avail) {
    return Status::corrupt("recipe: trailing garbage (" + std::to_string(body_avail - body_size) +
                           " bytes)");
  }

  const uint8_t* body = in.data() + kHeaderSize;
  // Must mirror encode() exactly: header bytes [0,60) then body, skipping the
  // CRC field at [60,64).
  uint32_t got_crc = crc32c(ByteSpan(in.data(), 60));
  got_crc = crc32c(ByteSpan(body, body_size), got_crc);
  if (got_crc != want_crc) {
    return Status::corrupt("recipe: body CRC mismatch (want " + std::to_string(want_crc) +
                           " got " + std::to_string(got_crc) + ")");
  }

  Recipe r;
  r.logical_size = logical_size;
  std::memcpy(r.file_sha256.data(), in.data() + 24, 32);
  r.name.assign(reinterpret_cast<const char*>(body), name_len);
  // Only now, after every length has been validated against real bytes, is it
  // safe to allocate proportionally to a number that came off the disk.
  r.entries.resize(static_cast<size_t>(chunk_count));

  const uint8_t* p = body + name_len;
  uint64_t sum = 0;
  for (size_t i = 0; i < r.entries.size(); i++) {
    std::memcpy(r.entries[i].fp.data(), p, 32);
    r.entries[i].length = get_u32le(p + 32);
    if (r.entries[i].length == 0 || r.entries[i].length > kMaxChunkLength) {
      return Status::corrupt("recipe: entry " + std::to_string(i) + " has bad length " +
                             std::to_string(r.entries[i].length));
    }
    sum += r.entries[i].length;
    p += kEntrySize;
  }
  // Internal consistency: the chunks must add up to the file they claim to be.
  // A recipe that passes its CRC but fails this was written by a buggy engine,
  // not damaged in transit -- worth distinguishing, and worth refusing either way.
  if (sum != logical_size) {
    return Status::corrupt("recipe: chunk lengths sum to " + std::to_string(sum) +
                           " but logical_size is " + std::to_string(logical_size));
  }
  return r;
}

}  // namespace dedupe
