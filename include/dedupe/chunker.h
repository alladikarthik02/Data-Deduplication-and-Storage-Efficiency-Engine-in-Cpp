// Content-defined chunking (FastCDC / Gear rolling hash).
//
// THE PROBLEM THIS SOLVES -- the boundary-shift problem.
//   Split a file into fixed 8 KiB blocks. Now insert ONE byte at the front.
//   Every subsequent block's contents shift by one, so every fingerprint
//   changes, and a deduplicating store sees a completely new file. Backup
//   dedup would be worthless. This single failure is the entire reason
//   content-defined chunking exists.
//
// THE IDEA.
//   Choose cut points from the CONTENT, not from the offset. Run a rolling hash
//   over the bytes and cut wherever the hash matches a mask. An insertion
//   perturbs only the chunks near it; downstream boundaries re-synchronize,
//   because they were never defined by position in the first place.
//
// WHY GEAR AND NOT RABIN.
//   Rabin fingerprinting needs a multiply and a modulo per byte plus a
//   slide-out table for the byte leaving a 48-byte window. Gear is
//       fp = (fp << 1) + GEAR[byte]
//   -- one shift, one add, one table lookup. The window is implicit: after k
//   shifts a byte's contribution has moved k bits left, so a mask that tests
//   bits 16..49 (ours does) is effectively looking at the last ~50 bytes. Same
//   window as Rabin, a fraction of the work.
//
// WHY FastCDC ON TOP OF GEAR (two refinements, both measurable):
//   1. NORMALIZED CHUNKING. Plain Gear produces an exponential chunk-size
//      distribution: masses of tiny chunks and a long tail of huge ones. Tiny
//      chunks are pure metadata overhead (48-byte record header + 36-byte recipe
//      entry each); huge ones dedup badly. FastCDC uses a STRICTER mask before
//      the target average (15 bits -> cut probability 1/32768, i.e. reluctant to
//      cut early) and a LOOSER one after (11 bits -> 1/2048, eager to cut late).
//      The distribution tightens around the target without changing the
//      content-defined property.
//   2. CUT-POINT SKIPPING. Never test for a cut inside the first MinSize bytes.
//      A sub-minimum chunk would be rejected anyway, so hashing those bytes is
//      wasted work -- skipping them removes ~25% of the hashing at our
//      parameters.
//
// THE COST OF REFINEMENT 2, STATED HONESTLY.
//   Skipping means the hash has only warmed up over a few bytes at position
//   MinSize, so cut decisions in the first ~50 bytes after MinSize depend on
//   fewer preceding bytes than later ones. That very slightly weakens
//   content-definedness. It is a real trade, and tests/test_chunker.cpp MEASURES
//   the resulting boundary-preservation rate rather than assuming it is fine.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dedupe/types.h"

namespace dedupe {

struct ChunkerParams {
  uint32_t min_size = 2u << 10;   // 2 KiB
  uint32_t avg_size = 8u << 10;   // 8 KiB  -- target
  uint32_t max_size = 64u << 10;  // 64 KiB

  // FastCDC's published masks for an 8 KiB target at normalization level 2.
  // The bit COUNT sets the cut probability (1/2^bits); the bit POSITIONS set the
  // effective window. Both matter, which is why these are the paper's values and
  // not "any number with 15 bits set".
  uint64_t mask_s = 0x0003590703530000ull;  // 15 bits -- used while len < avg
  uint64_t mask_l = 0x0000d90003530000ull;  // 11 bits -- used while len >= avg

  // Checked at construction rather than trusted. max_size in particular is not
  // a taste question: SPEC 3.4 packs a chunk length into 17 bits of the index
  // entry, so a chunk longer than 131071 bytes would be silently truncated in
  // the index -- corruption, not a performance bug.
  bool valid() const {
    return min_size > 0 && min_size < avg_size && avg_size < max_size &&
           max_size <= 131071 && mask_s != 0 && mask_l != 0;
  }
};

namespace chunk_detail {

// The gear table: one random 64-bit value per byte value.
//
// Generated at COMPILE time by splitmix64 from a fixed seed, rather than pasted
// in as 256 literals. Two reasons: the values are reproducible and their origin
// is auditable in four lines, and splitmix64 passes the statistical tests that
// matter here -- we need the HIGH bits well distributed, since those are the
// ones the masks test.
constexpr std::array<uint64_t, 256> make_gear() {
  std::array<uint64_t, 256> g{};
  uint64_t x = 0x2545F4914F6CDD1Dull;  // fixed seed: chunk boundaries must be
                                       // identical on every machine, forever
  for (auto& v : g) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    v = z ^ (z >> 31);
  }
  return g;
}

inline constexpr std::array<uint64_t, 256> kGear = make_gear();

}  // namespace chunk_detail

// Returns the LENGTH of the first chunk in `buf`.
//
// Pure and stateless: the rolling hash restarts at every chunk boundary, so a
// chunk's extent depends only on its own bytes. That is not an implementation
// detail -- it is what makes boundaries independent of file position, and it is
// what lets the streaming Chunker below cut without carrying hash state.
inline size_t find_cut_point(ByteSpan buf, const ChunkerParams& p) {
  const size_t n = buf.size();
  if (n <= p.min_size) return n;  // too short to cut: it is all one chunk

  // Two regions, two masks (normalized chunking).
  const size_t limit = std::min<size_t>(p.max_size, n);
  const size_t normal = std::min<size_t>(p.avg_size, limit);

  uint64_t fp = 0;
  size_t i = p.min_size;  // cut-point skipping: no test before MinSize

  for (; i < normal; i++) {  // reluctant region: 15-bit mask
    fp = (fp << 1) + chunk_detail::kGear[buf[i]];
    if ((fp & p.mask_s) == 0) return i;
  }
  for (; i < limit; i++) {  // eager region: 11-bit mask
    fp = (fp << 1) + chunk_detail::kGear[buf[i]];
    if ((fp & p.mask_l) == 0) return i;
  }
  return limit;  // no cut found: forced boundary at MaxSize (or end of input)
}

// Streaming chunker: feed it bytes in arbitrary-sized pieces, it emits chunks.
//
// The one non-obvious rule is WHEN a cut may be emitted. A cut is final only if
// we know more data could not change it:
//   * a real cut was found strictly inside the available bytes  -> final, because
//     the scan is left-to-right and later bytes cannot affect an earlier match;
//   * or we already hold MaxSize bytes                          -> final, because
//     the boundary is forced regardless of what follows.
// Otherwise we must wait: with fewer than MaxSize bytes and no match yet, more
// data may produce a cut further on, and the mask that applies past AvgSize
// depends on the total length.
//
// LIFETIME: the ByteSpan handed to `emit` is valid ONLY for the duration of that
// callback. It may point into the chunker's carry buffer or directly into the
// caller's buffer, and either can be invalidated by the next cut. Callers hash
// and write the chunk inside the callback, which is why this contract is worth
// having: it avoids a copy per chunk on the hot path.
//
// COPY BUDGET. The naive design -- append everything to an internal buffer, then
// chunk it -- copies every byte that passes through, and measured 1353 MB/s
// against 1877 MB/s for the raw scan: the copy was eating 28% (BENCHMARKS T2).
// A "skip the copy when the buffer is empty" fast path does NOT fix that, because
// the buffer is empty exactly once: after any feed there is almost always a
// partial chunk carried over. What works is to copy only the SPLICE REGION --
// the carry plus just enough new data to resolve the chunk that straddles the
// boundary -- and then chunk the rest of the caller's buffer in place. Per feed
// that is at most (carry + MaxSize) ~= 128 KiB copied instead of the whole read.
class Chunker {
 public:
  explicit Chunker(ChunkerParams p = {}) : p_(p) {}

  const ChunkerParams& params() const { return p_; }

  template <class F>
  void feed(ByteSpan data, F&& emit) {
    size_t pos = 0;  // bytes of `data` already consumed

    // PHASE 1 -- resolve the carry, if any.
    // Splice the carry together with MaxSize bytes of the new data. That length
    // guarantees progress past the carry: while pos <= carry_len the remaining
    // span is >= MaxSize, so every cut in that region is already final and the
    // loop cannot stop before clearing the carry.
    if (!carry_.empty()) {
      const size_t carry_len = carry_.size();
      const size_t take = std::min<size_t>(p_.max_size, data.size());
      carry_.insert(carry_.end(), data.begin(), data.begin() + static_cast<long>(take));
      const size_t consumed = cut_from(carry_, /*at_eof=*/false, emit);

      if (consumed >= carry_len) {
        pos = consumed - carry_len;  // the splice ran into `data` this far
        carry_.clear();
      } else {
        // Could not get past the carry, which can only happen when `data` was
        // itself smaller than MaxSize. Buffer the remainder and wait for more.
        carry_.insert(carry_.end(), data.begin() + static_cast<long>(take), data.end());
        if (consumed > 0) {
          carry_.erase(carry_.begin(), carry_.begin() + static_cast<long>(consumed));
        }
        return;
      }
    }

    // PHASE 2 -- zero-copy: cut directly out of the caller's buffer, and copy
    // only the leftover tail (always < MaxSize).
    const ByteSpan rest(data.data() + pos, data.size() - pos);
    const size_t consumed = cut_from(rest, /*at_eof=*/false, emit);
    carry_.assign(rest.begin() + static_cast<long>(consumed), rest.end());
  }

  // End of input: everything left is emitted, including a final short chunk.
  template <class F>
  void finish(F&& emit) {
    cut_from(carry_, /*at_eof=*/true, emit);
    carry_.clear();
  }

 private:
  // Emit every chunk whose boundary is already final; return the bytes consumed.
  template <class F>
  size_t cut_from(ByteSpan s, bool at_eof, F& emit) {
    size_t pos = 0;
    while (pos < s.size()) {
      const size_t avail = s.size() - pos;
      const size_t len = find_cut_point(ByteSpan(s.data() + pos, avail), p_);
      const bool final_cut = at_eof || len < avail || avail >= p_.max_size;
      if (!final_cut) break;
      emit(ByteSpan(s.data() + pos, len));
      pos += len;
    }
    return pos;
  }

  ChunkerParams p_;
  std::vector<uint8_t> carry_;  // invariant: always < 2 * MaxSize, usually < MaxSize
};

// Fixed-size chunking. NOT a production path -- it exists so the tests can
// demonstrate, with a number, the boundary-shift failure that motivates all of
// the above (SPEC R1.1).
inline size_t fixed_cut_point(ByteSpan buf, size_t size) {
  return std::min(size, buf.size());
}

}  // namespace dedupe
