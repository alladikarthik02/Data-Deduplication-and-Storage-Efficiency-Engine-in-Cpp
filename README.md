# Data Deduplication and Storage Efficiency Engine

A single-node, content-addressed **deduplicating storage engine in C++20**, written from
scratch with **no external dependencies** — the chunker, SHA-256, CRC32C, hash index,
on-disk formats, crash recovery and garbage collector are all implemented here.

Files are split into content-defined chunks, fingerprinted with SHA-256, and only chunks
the store has never seen are written. A background collector reclaims unreferenced chunks
while ingests and restores keep running.

```
7.36×   storage reduction across 8 full backup generations of a 2,000-file tree
0.99×   on unique random data — the control that proves the ratio is real
 ±20%   foreground p50/p90/p99 latency impact while the collector runs
  225   crash-injection points, 52 real process kills, 0 failures
  309   test runs per gate (103 tests × normal, ASan+UBSan, TSan)
```

**Contents** · [Quick start](#quick-start) · [CLI](#cli) · [How it works](#how-it-works) ·
[Design decisions](#design-decisions) · [Results](#results) · [Testing](#testing) ·
[Layout](#repository-layout) · [Documentation](#documentation) · [Limitations](#limitations)

---

## Why

A nightly backup is almost identical to the one before it, but a naïve store pays for
every copy. Deduplication fixes that, and the hard part is not the deduplication itself —
that is one hash lookup. The hard part is staying correct while:

- the process is **killed mid-write**,
- several threads are **writing at the same time**, and
- a collector is **deleting chunks underneath them**.

Those three constraints shaped every design decision below.

---

## Quick start

Everything builds and runs **inside Linux in Docker**. The correctness claims are about
`fsync` ordering, atomic `rename`, torn-tail recovery and `flock`, all of which behave
differently on macOS/APFS, so the container is the target platform rather than a
convenience.

**Requirements:** Docker. Nothing else — no system libraries, no package installs.

```bash
git clone https://github.com/alladikarthik02/Data-Deduplication-and-Storage-Efficiency-Engine-in-Cpp.git
cd Data-Deduplication-and-Storage-Efficiency-Engine-in-Cpp

./scripts/dev.sh ./scripts/check.sh        # build + run every test, all 3 configurations
```

Store a file, inspect the store, verify it end to end:

```bash
./scripts/dev.sh bash -c '
  S=/tmp/store
  ./build-none/dedupe --store $S ingest myfile.bin
  ./build-none/dedupe --store $S stats
  ./build-none/dedupe --store $S verify --deep
'
```

`scripts/dev.sh` builds the image on first use and bind-mounts the repo, so edits stay on
the host while compilation and execution happen on Linux.

---

## CLI

```
dedupe --store <dir> ingest <file> [name]   # add a file
dedupe --store <dir> restore <name> <out>   # rebuild it, byte-exact
dedupe --store <dir> cat <name>             # ...to stdout
dedupe --store <dir> rm <name>              # delete (space returns via gc)
dedupe --store <dir> gc [--daemon <secs>]   # reclaim space
dedupe --store <dir> ls                     # list stored files
dedupe --store <dir> stats                  # logical vs physical bytes, dedup ratio
dedupe --store <dir> verify [--deep]        # check the whole store
```

---

## How it works

```
  ingest                                          restore
  ──────                                          ───────
  file bytes                                      recipe: (fingerprint, length)*
      │                                                │
      ▼                                                ▼
  FastCDC chunker      cut points chosen by        index lookup
  (Gear rolling hash)  CONTENT, not offset         fingerprint → container:offset:length
      │                                                │
      ▼                                                ▼
  SHA-256              32-byte fingerprint         read + verify CRC + verify fingerprint
      │                                                │
      ▼                                                ▼
  index probe          hit → reuse, miss → write   whole-file SHA-256 check
      │
      ▼
  container            append-only, self-describing records
      │                fsync BEFORE the recipe is published
      ▼
  recipe               tmp → fsync → rename → fsync(dir)   ← the atomic commit
```

**Five nouns.** A **chunk** is a variable-sized piece of a file. Its **fingerprint** is the
SHA-256 of its bytes. **Containers** are append-only files holding chunk records. A
**recipe** is one file's ordered list of fingerprints — it holds no data. The **index**
maps fingerprint → location.

**The one idea everything rests on:** containers and recipes are the only authoritative
state. The in-memory index is a **cache**, rebuilt by scanning containers on every open.
That turns index durability from a correctness problem into a performance problem, and it
works because every chunk record on disk is self-describing: a 48-byte header carrying the
full fingerprint, the length and a CRC32C.

**Durability is an ordering rule, not a feature:** never publish a reference before the
data it points at is durable. The same rule appears at three levels — chunks before
recipes, file contents before the rename that names them, journal record before the
irreversible action.

---

## Design decisions

Each with its reasoning, and the alternative it was chosen over. Full arguments in
[`docs/SPEC.md`](docs/SPEC.md).

| choice | over | because |
|---|---|---|
| Content-defined chunking | fixed-size blocks | one byte inserted at offset 0 destroys **every** fixed-size block; CDC keeps 99.9% |
| FastCDC / Gear | Rabin fingerprinting | one shift + add + lookup per byte instead of multiply + modulo, same effective window |
| SHA-256 | xxHash / CityHash | content addressing makes "same fingerprint ⇒ same bytes" a *correctness* assumption; a collision is silent data loss |
| 128-bit index key | full 256-bit | halves index RAM; ~10⁻²¹ collision risk at 10⁹ chunks, and every read verifies the full fingerprint from disk |
| Epoch mark-and-sweep | reference counting | refcounts race with in-flight dedup hits and cannot self-heal — one lost decrement leaks forever, one lost increment corrupts |
| Robin Hood open addressing | `std::unordered_map` | 1.8× less memory than a *tuned* baseline, and no pointer chase per lookup |
| Backward-shift deletion | tombstones | tombstones never go away; a delete-heavy store degrades until it is rebuilt |
| 1.5× index growth | doubling | doubling leaves the table half empty on average; 1.5× holds the load factor between 0.6 and 0.9 |
| Per-thread containers | one shared container | removes the global append lock; costs restore locality, which is stated rather than hidden |
| Runtime CPU dispatch | `-march=native` | one portable binary that still gets hardware SHA-256 and CRC32C |

---

## Results

Every number is reproduced by one command, listed with the measurement in
[`docs/BENCHMARKS.md`](docs/BENCHMARKS.md).

**Storage reduction** — 2,000-file tree, 8 successive full backups, 3% of files modified
between generations (plus 1% created, 1% deleted), chunk payloads uncompressed:

| generation | 1 | 2 | 4 | 6 | 8 |
|---|---|---|---|---|---|
| logical | 467 MiB | 936 MiB | 1,877 MiB | 2,816 MiB | 3,766 MiB |
| physical | 470 MiB | 474 MiB | 483 MiB | 495 MiB | 511 MiB |
| ratio | 0.99× | 1.97× | 3.88× | 5.69× | **7.36×** |

Generation 1 lands just *under* 1.0× — correct, because a 48-byte record header on every
~9.3 KiB chunk costs ~0.5% when there is nothing to deduplicate. An engine reporting a
gain there would be one to distrust.

The ratio is set by **how many times you back up, not by how much changes**: on a separate
1,000-file run, raising churn 25-fold (1% → 25%) moved it only 7.14× → 6.84×, while going
from 4 generations to 12 moved it 3.86× → 10.69×. Content-defined chunking confines each
edit to the chunk or two containing it.

**Throughput and memory:**

| | measured | note |
|---|---|---|
| SHA-256 | 2,074 MB/s | 124 MB/s portable; 16.7× from runtime-dispatched ARMv8 SHA-2 |
| CRC32C | 8,056 MB/s | 2,085 MB/s portable table path |
| Chunker | 1,806 MB/s | within 4% of the zero-copy ceiling |
| Index | **51.5 B/entry** | vs 92.7 for a tuned `unordered_map` — 1.8× smaller |
| Ingest | 275 → 893 MB/s | 1 → 8 threads (3.25×); the remainder is `fsync`, not lock contention |

**Correctness:** boundary preservation after a 1-byte insert is **99.9%** for CDC against
**0.0%** for a fixed-size control; 225 crash-injection points produce 0 failures; GC
running flat out keeps foreground latency within ±20% of idle.

---

## Testing

```bash
./scripts/dev.sh ./scripts/check.sh          # all three configurations
./scripts/dev.sh ./scripts/check.sh none     # just the fast one
```

Three configurations, because ASan and TSan cannot coexist in one binary:

| configuration | catches |
|---|---|
| normal `-O2` | the code that is actually shipped and benchmarked |
| ASan + UBSan | memory safety, undefined behaviour |
| TSan | data races, lock-order inversions |

Randomized tests are seeded and print the seed, so a failure replays exactly:

```bash
DEDUPE_SEED=0x... ./build-none/test_gc_stress
```

Benchmarks and the crash sweep are run explicitly, never in the test gate:

```bash
./scripts/dev.sh ./build-none/bench_corpus    # storage-reduction numbers
./scripts/dev.sh ./build-none/bench_gc        # GC vs foreground latency
./scripts/dev.sh ./build-none/crashtest 20    # exhaustive crash sweep (slow)
```

---

## Repository layout

```
include/dedupe/     public headers — chunker, sha256, crc32c, index, container,
                    format, io, store, gc, gc_journal, status, types
src/                implementation: container, format, index, io, store, gc, gc_journal
tests/              13 suites, from unit tests to concurrent stress and crash recovery
bench/              chunker, hash, index, ingest, gc, and the storage-reduction corpus
tools/              dedupe CLI, and crashtest (kill -9 at the Nth syscall)
scripts/            dev.sh (run anything in the container), check.sh (build + test)
docs/               SPEC, CHALLENGES, BENCHMARKS, CLAIMS
scratch/            the ARMv8 SHA-2 spike that preceded the hardware backend
```

---

## Documentation

| | |
|---|---|
| [`docs/SPEC.md`](docs/SPEC.md) | Architecture, the essential function surface, **safety requirements S1–S14** (each row names the test that proves it), the task breakdown, and the holes found by attacking the design on purpose |
| [`docs/CHALLENGES.md`](docs/CHALLENGES.md) | **The engineering journal** — 17 defects with the reasoning that found them, including the wrong first diagnoses |
| [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) | Every number, with the command that produced it |
| [`docs/CLAIMS.md`](docs/CLAIMS.md) | What the headline claims assert vs. what was measured |

If you read one, read `CHALLENGES.md`. Highlights: a safety argument in the spec that was
**wrong** and destroyed live data under load; a stale cached file size that made the
collector delete a full container; 32 bytes of every recipe sitting outside the checksum;
two optimizations that measured as completely worthless; and three benchmarks that were
measuring themselves.

---

## Limitations

Stated here rather than left for a reader to discover. The reasoning for each is in
[`docs/SPEC.md`](docs/SPEC.md) §1 and §8.

- **Process-crash consistency is tested; power-loss durability is not.** The crash tests
  kill the process, so the OS page cache survives. Proving power-loss behaviour needs real
  hardware.
- **The index is RAM-resident**, roughly 5–6 GB per TB of unique data. That is the real
  ceiling on how much one machine can hold; a production system would tier it to disk
  behind a filter and a locality-preserving cache.
- **No compression**, deliberately, so the storage reduction is attributable to
  deduplication alone.
- **Mark-and-sweep reads every recipe per cycle**, so collector cost scales with total
  data rather than with garbage.
- **Per-thread containers scatter a file's chunks**, which trades restore locality for
  write throughput.
- No FUSE filesystem, no multi-node replication, no on-disk index tier.

---

## License

BSD 3-Clause. See [`LICENSE`](LICENSE).
