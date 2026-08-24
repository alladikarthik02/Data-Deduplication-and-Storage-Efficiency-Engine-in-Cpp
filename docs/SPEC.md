# `dedupe` — Technical Specification

**Project:** Data Deduplication and Storage Efficiency Engine (C++)
**Résumé:** `Karthik_Alladi_PureStorage_SWE_Grad_CPP_Storage_Systems.pdf`, Academic Project #1
**Status:** living document — edited as reality contradicts it (contradictions get logged in `CHALLENGES.md`)

---

## 0. How to read this document

- §1 says what this is and what it deliberately is not.
- §2 is the **contract with the résumé**: every bullet mapped to a testable requirement. If a bullet
  cannot be honestly satisfied by the finished code, the *bullet* changes — not the measurement.
- §3 is the architecture.
- §4 is the **essential function list** — the API/CLI surface that must exist for this to be the
  project the résumé claims.
- §5 is the **safety requirements** (S1–S14). This is the checklist to re-read at the end of every
  task: *"which of these did I just put at risk, and what proves I didn't break it?"*
- §6 is the testing strategy, §7 the task breakdown (the build order with explicit stop points),
  §8 the holes found by attacking this spec on purpose.

---

## 1. What we are building, and what we are deliberately NOT building

**Building.** A single-node, content-addressed deduplicating storage engine in C++20:

- Ingest a file → split into **content-defined chunks** → **fingerprint** each chunk (SHA-256) →
  store only chunks not already present → persist a per-file **recipe** (the ordered list of
  fingerprints needed to rebuild it).
- Restore a file from its recipe, byte-for-byte.
- Delete a file, then let a **background space-reclamation** thread find and free chunks no
  longer referenced by any file — *while ingests and restores keep running*.
- A **memory-efficient in-RAM fingerprint index**, benchmarked so we can state bytes-per-chunk and
  show throughput does not degrade as the store grows.

**NOT building (and why):**

| Not doing | Why |
|---|---|
| Compression of chunk payloads | The résumé's storage-reduction number must be attributable to **deduplication alone**. Adding zlib would inflate the ratio and make the claim unfalsifiable. (Compression belongs to résumé project #2, in-transit.) |
| A FUSE filesystem / POSIX file semantics | The bullet says *engine*, not filesystem. Mount semantics (partial writes, mmap, O_DIRECT) would triple the work and add nothing the bullet claims. |
| Distributed / multi-node replication | That is résumé project #2. Keeping them separate keeps each defensible. |
| An on-disk (tiered) fingerprint index | Documented as the honest scaling limit in §8.3 instead. Faking a Bloom-filter tier that never touches disk would be résumé décor. |
| Encryption, auth, multi-tenancy | Not claimed anywhere. |

---

## 2. Résumé bullets → testable requirements (the contract)

> **R1.** "Built a C++ deduplication engine that splits incoming data into content-defined chunks,
> fingerprints them, and stores only unique chunks, achieving about 4 times storage reduction on a
> representative backup dataset while keeping restore correctness intact."

| ID | Requirement | Proven by |
|---|---|---|
| R1.1 | Chunking is **content-defined** (not fixed-size): inserting bytes near the start of a file must NOT re-cut the whole file. | `test_chunker`: byte-insertion property test asserts ≥90% of downstream chunk boundaries are preserved. Fixed-size chunking would score ~0%. |
| R1.2 | Chunks are fingerprinted with SHA-256, implemented in-tree. | `test_sha256` vs NIST FIPS-180-4 published vectors + a long-message vector. |
| R1.3 | A chunk already present is stored **once**. | `test_store`: ingest the same file twice → physical bytes written on the 2nd ingest ≈ 0; `stats` reports dedup ratio ≈ 2.0. |
| R1.4 | ≥4.0× storage reduction on a **defined, reproducible** backup workload. | `bench/backup_corpus` generator + `dedupe stats`; the workload model is printed with the number, always. See §8.1. |
| R1.5 | Restore is **byte-exact**. | Round-trip property tests over randomized corpora + whole-file SHA-256 recorded at ingest and verified at restore. |

> **R2.** "Implemented background space reclamation that safely identifies and frees chunks no longer
> referenced by any file, running concurrently with reads and writes without blocking foreground
> operations."

| ID | Requirement | Proven by |
|---|---|---|
| R2.1 | GC runs on a **background thread**, triggered by policy or by `dedupe gc`. | `src/gc.cpp` + `test_gc`. |
| R2.2 | GC **frees** space: after deleting files, physical bytes on disk actually shrink. | `test_gc`: assert container bytes drop by the expected amount. |
| R2.3 | GC is **safe**: it never frees a chunk that any surviving recipe references. See **S2**. | `test_gc_stress`: concurrent ingest + delete + GC, then restore *every* surviving file and byte-compare. |
| R2.4 | GC runs **concurrently** with reads and writes. | `test_gc_stress` runs ingest and restore threads for the entire GC cycle with no external barrier. T6 laid the foundation: 4-thread ingest, concurrent restore-during-ingest, and concurrent `list`/`stats` during churn, all TSan-clean. |
| R2.5 | GC does not **block** foreground operations. | `bench/gc_latency`: measure foreground p50/p99/p99.9 ingest latency with GC idle vs GC running; assert p99 inflation stays under a stated bound, and report the real number. |

> **R3.** "Tuned the chunking and indexing path with a memory-efficient fingerprint index, holding
> deduplication throughput steady as the dataset grew, and validating correctness through an
> extensive automated test suite."

| ID | Requirement | Proven by |
|---|---|---|
| R3.1 | The index is **memory-efficient** in a measured, comparable sense. | ✅ `bench_index`: **51.5 B/entry vs 92.7** for a *tuned* `unordered_map<Digest32,Location>` (**1.80×**) and 140.7 for `unordered_map<string,Location>` (**2.73×**). RSS measured in a forked child per case, so allocator overhead is included. |
| R3.2 | Throughput does **not degrade** as the dataset grows. | ✅ `bench_index` scaling: **dedup throughput 2021 → 1925 MB/s (−4.8%) over a 16× dataset increase.** Index *latency* does grow 5.4× (cache, not algorithm — probe distance stays 12→19), and both numbers are reported rather than only the flattering one. |
| R3.3 | The **chunking/indexing path is tuned**, with before/after numbers. | Every optimization lands with a measurement in `docs/BENCHMARKS.md`. An optimization without a number does not go in. |
| R3.4 | "Extensive automated test suite." | `ctest` runs unit + property + fault-injection + concurrency + sanitizer suites. §6. |

**Integrity rule (carried over from previous projects):** if the finished engine cannot hit a number
on the résumé, we change the résumé, not the measurement. Every claim in this project must be
reproducible by a stranger running one command in the container.

---

## 2.5 Verified environment facts (measured in T0, not assumed)

Everything below was *checked by a running test* on the target platform before any
dependent code was written. `tests/test_harness.cpp` is the proof, and it stays in the
suite so a future toolchain change that breaks one of these fails loudly.

| Fact | Value | Why the spec depends on it |
|---|---|---|
| Target platform | Ubuntu 24.04, GCC 13, linux/**aarch64**, 8 CPUs (Docker on Apple silicon) | Everything below is measured here, not on macOS. |
| Byte order | little-endian | §3.3 stores lengths/offsets raw via `memcpy`. Big-endian would byte-swap every field. |
| `CHAR_BIT` / int widths | 8 / `uint64_t`=8B / `uint32_t`=4B | All on-disk offsets in §3.3 are stated in bytes. |
| `sizeof(Entry)` | **32 bytes**, `alignof` 8 → exactly 2 per 64-byte cache line | The whole memory-efficiency claim (R3.1). Silent padding to 40 B would make every RAM figure in §3.4 wrong by 25%. |
| `sizeof(ChunkHeader)` | **48 bytes**, no padding | §3.3's record layout and its offset table. |
| `std::atomic<uint32_t/uint64_t>` | lock-free | S7: if atomics fell back to a mutex, "GC never blocks the foreground" would be quietly false. |
| Location packing `c:16 \| o:27 \| l:17` | round-trips at every boundary; capacity = **8 TiB** | §3.4's addressability claim, verified arithmetically rather than asserted. |
| `__ARM_FEATURE_CRC32` at default `-march` | **not defined**, though `AT_HWCAP` says the CPU has CRC32 **and** SHA-2 | Forces **runtime** feature dispatch instead of `-march=armv8-a+crc`; see `CHALLENGES.md` B3. |
| SHA-256 throughput | scalar **123.9 MB/s** → ARMv8 SHA-2 **2073.9 MB/s** (16.7×) | Was the ceiling on all ingest throughput; see `CHALLENGES.md` B4 and `BENCHMARKS.md`. |
| CRC32C throughput | table **2084.7 MB/s** → ARMv8 CRC32 **8055.5 MB/s** (3.9×) | Makes per-read integrity verification affordable (§3.3). |
| Mean chunk size at Min/Avg/Max = 2K/8K/64K | **9294 B**, not 8192 — predicted 9348 B analytically, 0.58% error | Every memory/overhead figure in §3.4 derives from it; see `CHALLENGES.md` B5. |
| Chunks forced out at MaxSize (random data) | **0.00%** (model: 5.7e-13) | MaxSize is a bound for pathological input, not a size control. |
| Boundary preservation after a 1-byte insert at offset 0 | CDC **99.9%** vs fixed-size **0.0%** | R1.1, the property the whole engine rests on. |
| Chunking throughput | scan **1884.5 MB/s**; streaming **1805.9 MB/s**; chunk+SHA-256 **966.1 MB/s** | The ingest CPU budget. |

---

## 3. Architecture

### 3.0 The one idea

Ground truth lives on disk in two places, and **nothing else is authoritative**:

1. **Containers** — append-only files of self-describing chunk records (each carries its own
   fingerprint, length and CRC).
2. **Recipes** — one per ingested file: an ordered list of (fingerprint, length).

The in-memory **index** (fingerprint → location) is a **cache, not a source of truth**. It can be
rebuilt from scratch by scanning containers. This single decision buys a lot: index durability
becomes a *performance* concern rather than a *correctness* concern, and any crash that loses index
state is recoverable by a scan. (See S3.)

```
  ingest path                                            restore path
  ───────────                                            ────────────
  file bytes                                             recipe (fp, len)*
      │                                                       │
      ▼                                                       ▼
  ┌────────────┐  chunk boundaries                      ┌───────────┐
  │  chunker   │  (FastCDC / Gear rolling hash)         │  index    │ fp → (container, off, len)
  └─────┬──────┘                                        └─────┬─────┘
        ▼                                                     ▼
  ┌────────────┐  32-byte fingerprint                   ┌───────────┐
  │  SHA-256   │                                        │ container │ read + verify CRC + verify fp
  └─────┬──────┘                                        └─────┬─────┘
        ▼                                                     ▼
  ┌────────────┐  hit → reuse location                    output bytes
  │   index    │  miss → append to container                  │
  └─────┬──────┘                                              ▼
        ▼                                              whole-file SHA-256 check
  ┌────────────┐
  │ container  │  append-only, fsync before recipe publish
  └─────┬──────┘
        ▼
  ┌────────────┐
  │   recipe   │  tmp → fsync → rename → fsync(dir)   ← the atomic "commit"
  └────────────┘
```

### 3.1 Chunking — FastCDC, and why not fixed-size or Rabin

**Why not fixed-size.** Fixed 8 KiB blocks break on the *boundary-shift problem*: insert one byte at
offset 0 and every subsequent block's content shifts, so every fingerprint changes and dedup yields
nothing. This is the entire reason content-defined chunking exists, and R1.1 tests exactly it.

**Why content-defined.** Cut points are chosen by the *content* — a rolling hash over a sliding
window, cut when the hash matches a mask. An insertion perturbs only the chunks near it; boundaries
re-synchronize downstream. That property is what makes backup dedup work at all.

**Why FastCDC (Gear) over Rabin.** Rabin fingerprinting needs a 48-byte sliding window with
multiply/mod per byte plus a slide-out table. Gear hashing is `h = (h << 1) + GEAR[byte]` — one
shift, one add, one table lookup per byte, and the window is implicit in the shift width. FastCDC
adds two refinements we implement:

- **Normalized chunking (NC=2):** use a *stricter* mask before the average size (harder to cut) and a
  *looser* mask after (easier to cut). This tightens the chunk-size distribution around the target,
  which reduces both tiny chunks (metadata overhead) and giant chunks (poor dedup).
- **Cut-point skipping:** never test for a cut inside the first `MinSize` bytes; sub-minimum chunks
  are pure overhead, and skipping them removes that work from the hot loop.

**Parameters (v1):**

| Parameter | Value | Why |
|---|---|---|
| `MinSize` | 2 KiB | Below this, the 48-byte record header + 32-byte recipe entry become a large relative overhead. |
| `AvgSize` | 8 KiB | Industry-typical; balances dedup granularity against index size (index RAM scales as data/AvgSize). |
| `MaxSize` | 64 KiB | Bounds the worst case for incompressible/pathological data and keeps a chunk length in 17 bits (§3.3). |
| `MaskS` | 15 bits set | Strict mask, used while `len < AvgSize`. |
| `MaskL` | 11 bits set | Loose mask, used while `len ≥ AvgSize`. |

**`AvgSize` is the mask switch point, not the resulting mean.** Measured mean chunk size
is **9294 B**, matching a first-principles model (2048 skipped + 5602 expected in the
reluctant region + 1698 in the eager region = 9348 B) to within 0.58%. Everything in §3.4
below uses the measured 9294 B, not the nominal 8192. Full derivation and the consequences
in `CHALLENGES.md` B5; distribution table in `BENCHMARKS.md`.

**Streaming copy budget.** The chunker copies only the *splice region* — the carried
partial chunk plus MaxSize bytes of new data, provably enough to resolve the chunk
straddling a read boundary — and cuts the rest of the caller's buffer in place. The naive
copy-everything design measured 28% slower (`CHALLENGES.md` B6).

### 3.2 Fingerprinting — SHA-256, in-tree

**Why SHA-256.** Content addressing means "same fingerprint ⇒ same bytes" is a *correctness*
assumption. With a 256-bit cryptographic digest, the probability of an accidental collision across
even 10¹⁵ chunks is ~10⁻⁴⁵ — far below the probability of undetected disk/DRAM corruption, which is
the honest comparison. A non-cryptographic hash (xxHash, CityHash) would be far faster but is
trivially collidable, and a collision here means **silent data corruption**, not a slow lookup.

**Why in-tree, not OpenSSL.** No hidden dependencies (same rule as previous projects), and a
from-scratch SHA-256 is verifiable against published NIST vectors, which is a stronger correctness
story than "linked a library." Cost is ~250 lines.

**Two backends, one answer.** SHA-256 is implemented twice: a portable scalar path and
an ARMv8 SHA-2 extension path, chosen at **runtime** from `AT_HWCAP` (never by
`-march`, which would produce a binary that `SIGILL`s on CPUs without the extension).
The measured gap is 16.7×, and since fingerprinting is unavoidable work on every
ingested byte, that gap was the ceiling on the whole engine's throughput. The
correctness obligation this creates is explicit: the two backends must agree bit-for-bit
or a store written on one machine is unreadable on another, so they are differentially
tested against each other at every length across the block and padding boundaries — not
merely each checked against the NIST vectors. Same reasoning, same pattern, for CRC32C's
three implementations. (`CHALLENGES.md` B3, B4.)

**Threat model note:** this is not a security boundary. We assume a non-adversarial ingest stream. If
an attacker could choose ingested bytes *and* SHA-256 were broken, a chosen-collision could alias two
different chunks. SHA-256 has no known practical collisions; recorded here so the assumption is
explicit rather than accidental (see S8 for the mitigation we ship anyway).

### 3.3 Storage layout

```
<store>/
  STORE                 superblock: magic, version, params (Min/Avg/Max, mask bits), store UUID
  LOCK                  flock'd while a process has the store open  (S14)
  containers/
    c00000001.dat       append-only chunk records
    ...
  recipes/
    <name>.rcp          one per ingested file
  gc/
    journal             GC intent log: BEGIN_COMPACT / COMMIT / DONE records  (S3, S4)
```

**Chunk record (48-byte header + payload), little-endian:**

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `magic` = `'C','H','N','K'` — resynchronization point when scanning a damaged container |
| 4 | 4 | `length` — payload bytes |
| 8 | 4 | `crc32c` — of the payload, detects bit rot and torn tails |
| 12 | 4 | `flags` |
| 16 | 32 | `fp` — full SHA-256 of the payload |

The record is **self-describing on purpose**: a container alone is enough to rebuild the index, and a
torn tail (crash mid-append) is detectable — the recovery scan truncates at the first record whose
header is short, whose magic is wrong, or whose CRC fails.

**Recipe file:** 64-byte header (`magic`, version, logical file size, chunk count, whole-file
SHA-256, source-name length) + name bytes + `chunk_count × {fp[32], length u32}`.

Metadata overhead: 36 B per chunk ≈ **0.387%** of logical size at the measured 9294 B mean. We store the full 32-byte
fingerprint rather than an 8-byte index ID so a recipe stays meaningful after a full index rebuild,
and so restore can verify end-to-end (S1). The overhead is the price of that independence, and it is
written down here so it is a decision rather than an accident.

**Container size target:** 128 MiB (2²⁷ bytes) — the exact width of the offset field in §3.4.

### 3.4 The fingerprint index — memory-efficient, sharded, open-addressed

**Entry (32 bytes, exactly two per 64-byte cache line):**

```c++
struct Entry {          // 32 bytes
  uint64_t k0, k1;      // first 16 bytes of the SHA-256 (128-bit key)
  uint64_t loc;         // packed: container_id:16 | offset:27 | length:17  (60 bits used)
  uint32_t touch_epoch; // last epoch this chunk was written or dedup-hit   (GC safety, §3.6)
  uint32_t flags;
};
```

Design choices and their reasons:

- **Open addressing with linear probing, not chaining.** A `std::unordered_map` allocates a node per
  entry (~48–64 B) plus a bucket pointer array, and every lookup chases a pointer into cold memory.
  A flat array of 32-byte entries costs one cache miss for the probe and usually resolves in the same
  line. This is the single largest source of the "memory-efficient" claim (R3.1) and it will be
  *measured against that exact baseline*, not asserted.
- **128-bit truncated key, not the full 256-bit fingerprint.** Halves index RAM. Collision risk by
  the birthday bound at 10⁹ chunks: ~10¹⁸ / 2¹²⁹ ≈ 10⁻²⁰ — negligible. A *64-bit* key would NOT have
  been: at 10⁸ chunks that is ~10⁻³, which is a real corruption risk, so the width is a computed
  decision, not a round number. See S8 for the belt-and-braces check.
- **Packed 64-bit location.** `container_id:16` (65 536 containers) × 128 MiB = **8 TiB** addressable;
  `offset:27` exactly spans a 128 MiB container; `length:17` covers the 64 KiB max chunk. Capacity
  limits are therefore explicit and checked at insert time (S11), not discovered in production.
- **256 shards, keyed on the top byte of the fingerprint.** Each shard has its own `shared_mutex` and
  its own table. Two consequences that matter:
  1. **Fine-grained locking** — 256-way concurrent access; ingest threads collide rarely.
  2. **No global rehash cliff** — shards grow independently, so a resize stalls 1/256th of the
     keyspace instead of the whole engine. This is what makes R3.2 ("throughput steady as the dataset
     grows") true rather than aspirational.
- **Robin Hood probing, max load factor 0.90, growth factor 1.5, capacity NOT a power of two.**
  Each of those is a correction to an earlier design that measured badly (`CHALLENGES.md` B10).
  Plain linear probing costs ~`1/(1-α)²` probes on an unsuccessful lookup — the common case,
  since every new chunk misses — so 0.90 was unusable without Robin Hood's displacement
  ordering and early exit. Growth of 2× leaves the table oscillating between 0.45 and 0.90
  load; 1.5× narrows that to [0.60, 0.90]. Sharding does **not** smooth this: uniform hashing
  makes all 256 shards cross their threshold at nearly the same instant. The home slot uses
  Lemire's fastrange (a 64×64 multiply keeping the high half), which works for any capacity
  and consumes the high bits of the key.
- **Deletion is backward-shift, not tombstones.** GC erases entries, and tombstones never go
  away — they lengthen every probe chain until the table is rebuilt, so a delete-heavy store
  degrades. Backward-shift restores the table to exactly the state it would have had if the
  key had never been inserted.

**Measured** cost (RSS, 1M entries, `BENCHMARKS.md` T4): **51.5 B of RAM per stored chunk** at
load 0.706, against 92.7 for a tuned `std::unordered_map<Digest32, Location>` (**1.80×**) and
140.7 for the obvious `unordered_map<std::string, Location>` (**2.73×**). At the measured mean
chunk size of 9294 B that is **0.554% of logical data**, so 1 TiB of unique data needs
**≈5.7 GiB** of index. §8.3 is honest about what that means.

### 3.5 Concurrency model

- **Ingest:** N worker threads, each owning its **own active container** for appends. This is the
  decision that removes the obvious global bottleneck — a single shared append lock would serialize
  every writer. Per-thread containers cost some space fragmentation, which GC's copy-forward
  compaction (§3.6) later cleans up. Both halves of that trade are deliberate.
  Measured (`BENCHMARKS.md` T6): **275 → 893 MB/s, 3.25× on 8 cores.** Sub-linear because ingest is
  **fsync-bound** (~1.94 ms per fsync here) rather than CPU- or syscall-bound, and the fsyncs are
  the S4 guarantee rather than overhead.
- **Cross-thread durability.** An open container is held in a `ContainerSlot` — a per-container
  mutex plus two atomic watermarks, `visible` (flushed, readable by other threads) and `durable`
  (fsynced). A recipe publish calls `ensure_durable()` on **every container it referenced, dedup
  hits included**, not merely its own. Syncing only your own container makes S4 true within a
  thread and false across threads, which is a corrupted-backup bug rather than a lost-ingest one
  (`CHALLENGES.md` B11). `Store::forced_syncs()` makes it observable, and the test asserts both
  that a non-durable container *is* synced and that an already-durable one is *not*.
- **Buffered appends.** Container appends batch into 1 MiB writes, so an I/O error surfaces at
  `flush()`/`sync()`/`close()` rather than at `append()`. Safe — a recipe is only published after a
  successful sync — but it is an interface change, and it introduced a partial-write duplication
  bug that `write_all` now prevents by reporting bytes written on the error path
  (`CHALLENGES.md` B12).
- **Index:** 256 shard locks, `shared_mutex` — readers (dedup probes, restore lookups) share, writers
  take exclusive briefly.
- **Lock order (single global rule):** `shard lock` → `container writer lock`. Never the reverse. GC
  holds **at most one shard lock at a time**, ever. A violation is a deadlock, so this rule is
  asserted in debug builds rather than trusted.
- **Restore:** lock-free in the common case — a shared-lock probe, then a positional `pread` on an
  open container handle. Containers are held via reference-counted handles so GC cannot unlink a file
  out from under an in-flight reader (S2).

### 3.6 Background space reclamation

The hard problem, and the reason this is bullet #2 on the résumé.

**Why not reference counting.** Refcounts race: a chunk's count can hit zero while another thread is
mid-ingest and about to dedup-hit that exact chunk. The window between "count reached 0" and "chunk
deleted" is a data-loss window, and closing it needs a lock that the ingest hot path would have to
take on *every* chunk. Refcounting also can't self-heal — one lost decrement leaks forever, one lost
increment corrupts.

**What we do: epoch-based conservative mark-and-sweep.**

```
epoch_start = ++global_epoch          // Phase 0: fence
  ── every write or dedup-hit stamps entry.touch_epoch = global_epoch ──
Phase 1  MARK    scan every recipe on disk; mark each referenced fingerprint
Phase 2  SELECT  candidates = unmarked AND entry.touch_epoch < epoch_start
Phase 3  COMPACT for each container with live_bytes/total < 0.5:
                   write live chunks to a NEW container, fsync
                   journal COMMIT(src → dst), fsync            ← the durable decision point
                   repoint index entries (one shard lock at a time)
                   drop container refcount; unlink src when the last reader releases it
                   journal DONE
```

**The safety argument, stated so it can be attacked:** a chunk is freed only if *no recipe references
it* **and** *nothing touched it since the epoch fence*. Any chunk written or hit during the GC cycle
has `touch_epoch ≥ epoch_start` and is therefore ineligible, even if the mark phase never saw it.
A recipe published mid-scan may be missed by MARK — but every chunk it references was necessarily
touched after the fence, so the epoch clause saves it. The failure mode of this design is
**retaining garbage for one extra cycle**, never freeing live data. Conservatism is deliberately
aimed in the direction where a bug costs space, not data.

**Crash safety of compaction:** the source container is unlinked only *after* the COMMIT record is
durable. Crash before COMMIT → the orphaned destination is discarded on recovery, source is intact.
Crash after COMMIT, before DONE → recovery re-points the index (rebuilt by scanning) and finishes the
unlink. Both paths are idempotent.

**Not blocking the foreground (R2.5):** GC's per-lock critical sections are bounded to a fixed batch
of entries; it never holds a shard lock across I/O; it never takes two shard locks. Measured, not
assumed — `bench/gc_latency` reports foreground p99 with GC on vs off.

---

## 4. Essential functions — the required surface

If any of these is missing, this is not the project the résumé describes.

### 4.1 Library API (`include/dedupe/`)

| Function | Contract |
|---|---|
| `Chunker::next(span) → optional<Chunk>` | Streaming, content-defined; respects Min/Avg/Max; deterministic for identical input. |
| `sha256(span) → Digest32` / incremental `Sha256::update/final` | Matches NIST vectors. |
| `Store::open(path, Options) → Result<Store>` | Opens or creates; acquires `LOCK`; recovers torn tails; rebuilds index if needed. |
| `Store::ingest(name, istream) → Result<IngestStats>` | Chunk → fingerprint → dedup → append → publish recipe atomically. Returns logical bytes, physical bytes written, chunks new/deduped. |
| `Store::restore(name, ostream) → Result<void>` | Byte-exact reconstruction; verifies per-chunk CRC + fingerprint and whole-file digest. |
| `Store::remove(name) → Result<void>` | Deletes the recipe only. Space comes back via GC, never inline. |
| `Store::list() → vector<FileInfo>` | Names, logical sizes, chunk counts. |
| `Store::stats() → StoreStats` | Logical bytes, physical bytes, dedup ratio, unique/total chunks, index bytes/chunk, container count, live fraction. |
| `Store::verify(Level) → Result<VerifyReport>` | **The oracle.** Full consistency check: every recipe's chunks resolve; every chunk's CRC and fingerprint match its content; no index entry points outside a container; no recipe references a freed chunk. Used by tests, and shippable as a command. |
| `Gc::start() / stop() / run_once() → GcStats` | Background thread + a synchronous single cycle for deterministic testing. |
| `Index::insert/lookup/repoint/erase`, `Index::memory_bytes()` | Sharded, epoch-stamping, capacity-checked; exposes its own footprint for R3.1. |

### 4.2 CLI (`dedupe`)

```
dedupe --store <dir> ingest <file>...      # add files
dedupe --store <dir> restore <name> <out>  # rebuild a file
dedupe --store <dir> rm <name>             # delete a recipe
dedupe --store <dir> gc [--once|--daemon]  # reclaim space
dedupe --store <dir> stats                 # ratio, footprint, index cost
dedupe --store <dir> ls                    # list stored files
dedupe --store <dir> verify [--deep]       # consistency oracle
dedupe --store <dir> bench <subcommand>    # reproducible measurements
```

### 4.3 Tools

| Tool | Purpose |
|---|---|
| `bench/gen_backup_corpus` | Generates the reproducible "representative backup dataset" of §8.1 from a seed. |
| `bench/index_mem` | Our index vs `std::unordered_map` baseline, both measured (R3.1). |
| `bench/scaling` | Throughput at growing dataset sizes (R3.2). |
| `bench/gc_latency` | Foreground latency with GC idle vs running (R2.5). |
| `tools/crashtest` | Forks a child, kills it at a chosen sync point, reopens the store, verifies (S3). |

---

## 5. Safety requirements — the checklist to re-read after every task

Each item: **what must never happen**, **how it is enforced**, **what proves it**.

| ID | Invariant | Enforcement | Proof |
|---|---|---|---|
| **S1** | **Restored data is never wrong.** `restore(ingest(F)) == F`, byte for byte. | Per-chunk CRC32C + full fingerprint verified on read; whole-file SHA-256 stored at ingest and checked at restore; any mismatch is a hard error, never a warning. | Round-trip property tests over randomized corpora (all sizes incl. 0, 1, exactly Min/Avg/Max, Max+1); `verify --deep`. |
| **S2** | **GC never frees a referenced chunk.** | Conservative epoch mark-and-sweep (§3.6); reference-counted container handles so an in-flight reader's file is never unlinked; COMMIT-before-unlink. | `test_gc_stress`: concurrent ingest/restore/delete during GC, then restore *every* surviving file and byte-compare; plus a targeted test for the "delete → GC starts → re-ingest same content" resurrection race. |
| **S3** | **A crash never corrupts the store.** After `kill -9` at any point, the store opens and every *completed* operation is intact. | Self-describing chunk records + CRC → torn tails are detected and truncated; recipes published by `tmp → fsync → rename → fsync(dir)`; GC journal with idempotent replay; index rebuildable by container scan. | `tools/crashtest` at ≥8 injection points × N iterations, each followed by open + `verify --deep` + restore-all. |
| **S4** | **A reference is never published before its data is durable.** | Container `fsync` precedes recipe rename; directory `fsync` follows rename; GC COMMIT `fsync` precedes source unlink. | Ordering asserted by a fault-injection I/O layer that fails at each sync boundary; crashtest covers the same points. |
| **S5** | **No memory-safety bugs.** | Bounds checks on every length read from disk *before* it is used to size or index anything. | Full suite under ASan + UBSan in CI script (`scripts/check.sh`). |
| **S6** | **No data races, no deadlocks.** | Sharded locking with one documented global lock order (§3.5); GC holds ≤1 shard lock; debug-build lock-order assertions. | Full concurrency suite under TSan; stress test with N ingest threads + M restore threads + GC. |
| **S7** | **GC never blocks the foreground unboundedly.** | Bounded batch size per lock acquisition; no I/O under a shard lock; no global stop-the-world phase. | `bench/gc_latency` reports p50/p99/p99.9 with GC on vs off; the regression bound is asserted in a test. |
| **S8** | **A fingerprint collision never silently aliases two different chunks.** | 128-bit index key (birthday-bounded, §3.4) + on an index hit the stored 32-byte fingerprint in the chunk header is compared before reuse in `--paranoid` mode; a mismatch stores the chunk as new and logs loudly. | Forced-collision test: inject two distinct chunks whose truncated keys are made to collide, assert both are stored and both restore correctly. |
| **S9** | **Corrupt or hostile store files never crash the process.** | Every parsed length/offset validated against the file's real size before use; readers return errors, never assert or UB. | Corruption fuzz: flip/truncate/extend random bytes in containers, recipes, superblock and journal; every case must yield a clean error under ASan. |
| **S10** | **Memory stays bounded and predictable.** | Streaming ingest (never load a whole file); bounded work queues; index footprint reported and asserted per chunk. | `bench/index_mem` + a test that ingests a file far larger than RAM budget with a hard RSS ceiling. |
| **S11** | **No silent failure.** Every syscall checked; short writes retried; capacity limits (container count, offset, length) checked at insert, not overflowed. | `Result<T>` return type everywhere on the I/O path; no ignored return values (`-Wunused-result` as an error). | Fault-injection I/O layer fails the Nth op of each kind; the engine must surface an error and leave the store `verify`-clean. |
| **S12** | **The user's source data is never modified or deleted.** Ingest opens inputs read-only; destructive operations touch only paths inside the store directory. | Inputs opened `O_RDONLY`; every unlink path is asserted to be within the store root. | `test_store`: source file mtime/content unchanged after ingest; a unit test that a crafted recipe name cannot escape the store dir (`../`, absolute paths). |
| **S13** | **Randomized tests are reproducible.** | Every randomized test is seeded and prints its seed on failure; no wall-clock or `random_device` in test logic. | Reviewed per test; a failing seed can be replayed by `--seed`. |
| **S14** | **Two processes never write the same store concurrently.** | `flock` on `<store>/LOCK` held for the lifetime of `Store`; second opener fails cleanly. | `test_store`: second `Store::open` on the same path returns an error rather than corrupting. |

**Ritual:** at the end of every task in §7, walk this table and answer *"did this task touch S-n, and
what test now covers it?"* Any "not covered" becomes an entry in `CHALLENGES.md` or a new test.

---

## 6. Testing strategy

| Layer | What it catches | Where |
|---|---|---|
| **Unit** | SHA-256 vs NIST vectors; CRC32C vs known values; chunker Min/Max enforcement; index insert/lookup/erase/grow invariants; loc packing round-trip. | `tests/test_sha256.cpp`, `test_crc32c.cpp`, `test_chunker.cpp`, `test_index.cpp` |
| **Property / randomized** | Round-trip fidelity across sizes and content classes; CDC boundary-shift preservation; dedup ratio ≈ 1.0 on incompressible unique data (proves we are not fooling ourselves). | `tests/test_roundtrip.cpp` |
| **Differential** | Our chunker vs an independent, deliberately naïve reference implementation over random inputs — same boundaries or the test fails. | `tests/test_chunker.cpp` |
| **Fault injection** | Every I/O error path: short writes, `ENOSPC`, failed `fsync`, failed `rename`. | `tests/test_faults.cpp` + `include/dedupe/io.h` injection hooks |
| **Crash / recovery** | Torn container tails, half-published recipes, interrupted GC compaction. | `tools/crashtest` + `tests/test_recovery.cpp` |
| **Concurrency** | Races and deadlocks under N-writer / M-reader / GC load. | `tests/test_concurrent.cpp`, `test_gc_stress.cpp` (also built under TSan) |
| **Corruption fuzz** | Hostile/damaged store files. | `tests/test_corrupt.cpp` (under ASan) |
| **Sanitizers** | UB, leaks, races. | `scripts/check.sh` builds three configurations: normal, ASan+UBSan, TSan — all suites run in each. |
| **Benchmarks** | The résumé's numbers. | `bench/*`, results recorded in `docs/BENCHMARKS.md` with the exact command that produced them. |

**Rule:** every task in §7 ends with `ctest` green, and no task is "done" while a suite is red.

---

## 7. Task breakdown — the build order

Each task is a stop point: build green, tests green, then an explanation before moving on.

| # | Task | Deliverable | Gates |
|---|---|---|---|
| **T0** | Foundations | Repo, Dockerfile, `scripts/dev.sh`, CMake + CTest, test harness, this spec, `CHALLENGES.md` | A trivial suite runs green **inside the container** |
| **T1** | Fingerprint | `sha256.h`, `crc32c.h` | NIST vectors pass; incremental == one-shot |
| **T2** | Chunker | `chunker.h` (FastCDC/Gear) | Min/Max respected; boundary-shift property ≥90%; differential vs reference; size distribution reported |
| **T3** | On-disk formats + I/O layer | `io.h` (checked, injectable), `format.h`, container writer/reader, recipe writer/reader | Torn-tail recovery test; corruption test; S9/S11 covered |
| **T4** | Index | `index.h` — sharded open-addressed table, epoch stamping | Invariants + growth tests; `memory_bytes()` reported; baseline comparison |
| **T5** | Store: ingest + restore | `store.h/.cpp`, minimal CLI | End-to-end round-trip property tests; dedup on second ingest ≈ 0 new bytes; S1/S12/S14 |
| **T6** | Concurrency | Multi-threaded ingest, per-thread containers, restore under load | TSan clean; throughput vs thread count recorded |
| **T7** | Space reclamation | `gc.h/.cpp` — epoch mark-sweep + copy-forward compaction + journal | S2 stress test; space actually shrinks; foreground latency measured (R2.5) |
| **T8** | Crash & fault injection | `tools/crashtest`, fault hooks wired through | ≥8 injection points survive open + `verify --deep` + restore-all |
| **T9** | Benchmarks & the résumé numbers | `bench/*`, `docs/BENCHMARKS.md` | Dedup ratio on the defined corpus; scaling curve; index bytes/chunk; GC latency |
| **T10** | Hardening & docs | `README.md`, `verify --deep`, S-table sweep, résumé bullet reconciliation | Every S1–S14 row has a named passing test; bullets rewritten if the numbers say so |

---

## 8. Holes in this spec (found by attacking it on purpose)

### 8.1 "About 4× storage reduction" is meaningless without the workload

A dedup ratio is a property of the *data*, not the engine. Ten full backups of a slowly-changing tree
dedup ~9×; ten tarballs of unrelated random bytes dedup 1.0×. Quoting a bare number is the most
common way storage benchmarks lie.

**Therefore:** the number is only ever reported alongside its generator. The v1 "representative
backup dataset" is defined as: a synthetic tree of ~2 000 files (mixed sizes, mixed text/binary-like
content) captured as **8 successive full backups**, where between generations ~3% of files are
modified (in-place edits, appends, truncations), ~1% created, ~1% deleted. This models the real thing
backup dedup is bought for: repeated full backups of data that barely changes. We will report the
measured ratio for that workload **and** for two controls — unique random data (expect ≈1.0×) and a
single generation (expect ≈1.0–1.2×) — so the number is bounded on both sides. If 8 generations do
not reach 4×, we say so and fix the bullet.

### 8.2 Per-thread containers fragment space

Giving each ingest thread its own active container removes the append bottleneck but leaves partially
filled containers and scatters a single file's chunks across several files, hurting restore locality.
GC's copy-forward compaction reclaims the space; restore locality is *not* fixed by v1 and is
recorded here as a known cost, not a discovered surprise. (Real systems solve this with
stream-informed layout — chunks of one write stream kept adjacent. Out of scope, named on purpose.)

### 8.3 The index is RAM-resident — that is the real scaling ceiling

~43 B per unique chunk means 1 TiB of unique data needs ~5.6 GiB of index. Production dedup systems
(Data Domain and friends) attack this with on-disk index tiers, Bloom filters, sparse indexing and
locality-preserving caches. We are **not** building that; §1 says so. What we *will* do is measure
bytes/chunk honestly and state the ceiling. A résumé claim of "memory-efficient index" that means
"5× smaller than the naïve `unordered_map`, measured" is defensible; one that implies petabyte-scale
tiering is not.

### 8.4 Mark-and-sweep costs a full recipe scan

Every GC cycle reads every recipe: O(total chunk references), not O(garbage). Fine at our scale,
wrong at Data Domain's — they use perfect-hash bitmaps over the whole fingerprint space. Named here
so the limitation is a known trade, and so the interview answer exists.

### 8.5 Epoch conservatism retains garbage for one cycle

A chunk that becomes garbage *during* a GC cycle is protected by its `touch_epoch` and survives to the
next cycle. That is the price of never needing a stop-the-world barrier, and it is the correct
direction to be wrong in.

### 8.6 `fsync` inside Docker on macOS is not a power-loss durability guarantee

The container's writes land in a VM (virtiofs/gRPC-FUSE) on top of APFS. We can honestly test
**process-crash consistency** (kill -9 → reopen → verify), and we do. We cannot honestly test
**power-loss durability** on this hardware. The distinction is stated in the README rather than
quietly blurred, because "we fsync" and "we survive power loss" are different claims and only one of
them is testable here.

### 8.7 A 128-bit truncated key still relies on SHA-256's uniformity

The birthday math in §3.4 assumes fingerprints are uniformly distributed — true for SHA-256, false
for a weak hash. The number and the hash choice are therefore coupled; changing the hash invalidates
the key width. Written down so nobody "optimizes" the hash later without redoing the arithmetic.

### 8.8 Chunk-size distribution is an assumption until measured — ✅ RESOLVED in T2

All the RAM/overhead arithmetic above assumed the chunker produces ~8 KiB average chunks.
It does not: the measured mean is **9294 B**, because `AvgSize` names the mask switch point
rather than a target. The assumption was wrong by 13% and every derived figure in §3.4 has
been corrected (all of them moved favourably). The measured distribution — mean, p1, p50,
p99, max, `at_max`, `at_min` — is in `BENCHMARKS.md`, reproduced by `test_chunker`, and the
analytic model that predicts it to within 0.58% is in `CHALLENGES.md` B5.

This hole existing in the spec is why the number got checked instead of being discovered
later as a capacity-planning failure.
