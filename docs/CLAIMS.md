# Claims reconciliation

The project was built to make three headline claims true and checkable. This document
compares each claim against what the finished engine actually does, and proposes wording
that a reader could verify.

**The rule, set in `SPEC.md` §2 before any code was written:** if the finished engine
cannot hit a number in a claim, the *claim* changes, not the measurement. That rule
cuts both ways, and here it cut the other way — one claim turned out to be conservative.

Every number below is reproduced by one command in `BENCHMARKS.md`.

---

## Bullet 1 — deduplication and storage reduction

> Built a C++ deduplication engine that splits incoming data into content-defined chunks,
> fingerprints them, and stores only unique chunks, achieving about 4 times storage
> reduction on a representative backup dataset while keeping restore correctness intact.

**Verdict: true, and understated — but not currently checkable.**

| clause | evidence |
|---|---|
| content-defined chunks | FastCDC/Gear, normalized chunking, cut-point skipping. One byte inserted at offset 0 of an 8 MiB corpus: **CDC preserves 99.9% of chunk fingerprints, fixed-size 8 KiB preserves 0.0%**. |
| fingerprints them | SHA-256 in-tree, validated against the NIST vectors; runtime-dispatched ARMv8 SHA-2 backend, **2074 MB/s**. |
| stores only unique chunks | A second ingest of identical content writes **0 bytes**. |
| ~4× storage reduction | **7.36×** over 8 full backup generations of a 2000-file tree at 3% churn. 4× is reached at **~4 generations** (3.88×). |
| restore correctness intact | Byte-exact round trips across every chunker size class; whole-file SHA-256 verified on every restore; `verify --deep` over the store after every benchmark run. |

**The problem with the current wording** is not the number, it is that "about 4 times on a
representative backup dataset" cannot be checked by anyone, including me. A dedup ratio is
a property of the data: the same engine gives 0.99× on unique random data and 10.69× at 12
generations. A ratio without its workload is not a claim.

**Proposed rewrite:**

> Built a C++ content-addressed deduplication engine (FastCDC chunking, SHA-256
> fingerprints, no compression) that achieved **7.4× storage reduction across 8 full backup
> generations of a 2,000-file tree** with 3% of files changing between backups — and 0.99×
> on unique random data, verifying the ratio comes from deduplication rather than the
> benchmark; restores are byte-exact and checked end-to-end by a whole-file digest.

---

## Bullet 2 — background space reclamation

> Implemented background space reclamation that safely identifies and frees chunks no
> longer referenced by any file, running concurrently with reads and writes without
> blocking foreground operations.

**Verdict: true, and it is the hardest part of the project.**

| clause | evidence |
|---|---|
| background | `Gc::start()/stop()` runs on its own thread; `run_once()` exists so tests are deterministic. |
| safely | Epoch mark-and-sweep with a **reclaim watermark** held to the oldest in-flight ingest. Deliberately not refcounting: refcounts race with in-flight dedup hits and cannot self-heal. Proven by 3 ingest + 2 restore threads against a live collector, byte-comparing every survivor. |
| frees chunks | Containers with no live chunks are unlinked; containers below 50% live are copy-forward compacted. 3.5 MB → 0.4 MB in the deletion test. |
| concurrently with reads and writes | The stress suites never quiesce anything; TSan-clean. |
| without blocking foreground | **Within ±20% of GC-idle at p50/p90/p99 for both ingest and restore**, often *better*, holding at 316 GC cycles during the measurement window. |

**Worth being able to tell the story of:** this bullet cost three real defects, all found by
the stress suite rather than by inspection — a **wrong safety argument in my own spec**
(an epoch fence protects operations that *start* after it, and says nothing about ingests
already in flight), a **stale cached file size** that made a full container scan as empty
so the collector deleted live data, and a crash window that left a zero-byte container and
made the store refuse to open at all.

**Proposed rewrite:**

> Implemented concurrent background space reclamation — epoch-based mark-and-sweep with
> copy-forward container compaction and a write-ahead intent journal — that frees
> unreferenced chunks while ingests and restores run, holding foreground p50/p90/p99
> latency **within ±20% of idle**; crash-safe across **225 fault-injection points** covering
> nine syscall classes.

---

## Bullet 3 — the tuned index and the test suite

> Tuned the chunking and indexing path with a memory-efficient fingerprint index, holding
> deduplication throughput steady as the dataset grew, and validating correctness through
> an extensive automated test suite.

**Verdict: true.**

| clause | evidence |
|---|---|
| tuned the chunking path | Splice-region copy in the streaming chunker: **1353 → 1806 MB/s (+33%)**, within 4% of the zero-copy ceiling. Runtime-dispatched hardware SHA-256: **124 → 2074 MB/s (16.7×)**, which was the ceiling on all ingest throughput. |
| memory-efficient fingerprint index | 256 shards, 32-byte entries, Robin Hood probing at 0.90 load, 1.5× growth. **51.5 B/entry vs 92.7** for a *tuned* `unordered_map<Digest32,Location>` (**1.80×**) and 140.7 for the obvious one (**2.73×**). Measured at **41.2 B/chunk** on the real backup corpus. |
| throughput steady as the dataset grew | Dedup throughput **2021 → 1925 MB/s (−4.8%) over a 16× dataset increase**. Index *latency* does grow 5.4× from cache effects; both numbers are reported. |
| extensive automated test suite | **103 tests in 13 suites, each run in three configurations** (normal, ASan+UBSan, TSan) = 309 runs per gate, plus a 225-point crash sweep. |

**Proposed rewrite:**

> Tuned the ingest path with measured before/after at every step — runtime-dispatched
> ARMv8 SHA-256 (**16.7×**, it was the throughput ceiling) and a splice-region streaming
> chunker (**+33%**) — behind a sharded Robin Hood fingerprint index costing **51.5 B/chunk,
> 1.8× less than a tuned `std::unordered_map`**; deduplication throughput held within 5%
> across a 16× dataset increase, validated by 103 tests run in three sanitizer configurations.

---

## Claims not made, and could have been

- **Crash safety.** 225 injection points across nine syscall classes, 52 real process
  kills, **zero failures**, where the assertion is that *presence implies correctness*: a
  file may be missing after a crash, but a file that exists and restores wrong is data loss.
- **Adversarial input handling.** Every on-disk decoder is bounds-checked before it sizes
  an allocation; 3000 randomly mutated recipes accepted only when self-consistent; every
  one of a recipe's 252 bytes flipped and every flip caught.
- **The engineering journal.** `docs/CHALLENGES.md` documents 17 defects with the reasoning
  that found them — including four where the first diagnosis was wrong, two optimizations
  that measured as worthless, and three benchmarks that measured themselves. That is more
  interesting in an interview than any of the numbers.

---

## Honest limits — say these before you are asked

- **Process-crash consistency, not power-loss durability.** The crash tests kill the
  process; the machine survives, so bytes already handed to `write()` remain in the page
  cache. Testing power loss needs hardware this project does not have. (SPEC §8.6)
- **The index is RAM-resident.** ~41 B per unique chunk means ~4.4 GiB per TiB of unique
  data. Production systems tier the index to disk with Bloom filters and sparse indexing;
  this one does not, and the ceiling is stated rather than hidden. (SPEC §8.3)
- **Mark-and-sweep scans every recipe** — O(total chunk references) per cycle, not
  O(garbage). Fine at this scale, wrong at Data Domain's, who use perfect-hash bitmaps.
  (SPEC §8.4)
- **Single node.** Replication is a different project.
- **All numbers are from one machine** — Docker on Apple silicon, `linux/aarch64`, 8 CPUs.
  The ratios (hardware vs software, ours vs baseline) are the durable results; the absolute
  MB/s are specific to this host.
