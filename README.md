# `dedupe` — Data Deduplication and Storage Efficiency Engine (C++20)

A single-node, content-addressed deduplicating storage engine, written from scratch with
**no external dependencies**. Files are split into content-defined chunks, fingerprinted
with SHA-256, and only chunks not already present are stored. A background collector
reclaims chunks no file references any more, while ingests and restores keep running.

```
7.36×  storage reduction across 8 full backup generations of a 2,000-file tree
0.99×  on unique random data — the control that proves the ratio is real
 ±20%  foreground p50/p90/p99 latency impact while the collector runs
  225  crash-injection points, 52 real process kills, 0 failures
  103  tests, each run in 3 configurations (normal, ASan+UBSan, TSan) = 309 runs
```

---

## Try it

Everything builds and runs **inside Linux in Docker** — the correctness claims are about
`fsync` ordering, atomic `rename`, torn-tail recovery and `flock`, all of which behave
differently on macOS/APFS.

```bash
./scripts/dev.sh ./scripts/check.sh          # build + test, all 3 configurations
```

```bash
./scripts/dev.sh bash -c '
  S=/tmp/store
  ./build-none/dedupe --store $S ingest myfile.bin
  ./build-none/dedupe --store $S stats
  ./build-none/dedupe --store $S verify --deep
'
```

```
dedupe --store <dir> ingest <file> [name]   # add a file
dedupe --store <dir> restore <name> <out>   # rebuild it, byte-exact
dedupe --store <dir> cat <name>             # ...to stdout
dedupe --store <dir> rm <name>              # delete (space returns via gc)
dedupe --store <dir> gc [--daemon <secs>]   # reclaim space
dedupe --store <dir> ls | stats | verify [--deep]
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

**The one idea everything rests on:** containers and recipes are the only authoritative
state. The in-memory index is a **cache**, rebuilt by scanning containers on every open.
That turns index durability from a correctness problem into a performance problem.

Design decisions, each with its reasoning, are in [`docs/SPEC.md`](docs/SPEC.md). The short
version:

| choice | over | because |
|---|---|---|
| Content-defined chunking | fixed-size blocks | one byte inserted at offset 0 destroys **every** fixed-size block; CDC keeps 99.9% |
| FastCDC / Gear | Rabin fingerprinting | one shift + add + lookup per byte instead of multiply + modulo, same effective window |
| SHA-256 | xxHash / CityHash | content addressing makes "same fingerprint ⇒ same bytes" a *correctness* assumption; a collision is silent data loss |
| Epoch mark-and-sweep | reference counting | refcounts race with in-flight dedup hits and cannot self-heal — one lost decrement leaks forever, one lost increment corrupts |
| Robin Hood open addressing | `std::unordered_map` | 1.8× less memory than a *tuned* baseline, and no pointer chase per lookup |
| Backward-shift deletion | tombstones | tombstones never go away; a delete-heavy store degrades until it is rebuilt |
| Runtime CPU dispatch | `-march=native` | one portable binary that still gets the 16.7× hardware SHA-256 |

---

## Documentation

| | |
|---|---|
| [`docs/SPEC.md`](docs/SPEC.md) | Architecture, the essential function surface, **safety requirements S1–S14** (each row names the test that proves it), the task breakdown, and the holes found by attacking the design |
| [`docs/CHALLENGES.md`](docs/CHALLENGES.md) | **The engineering journal** — 17 defects with the reasoning that found them, including the wrong first diagnoses |
| [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) | Every number, with the command that produced it |
| [`docs/RESUME.md`](docs/RESUME.md) | What the résumé claims vs. what was measured |

If you read one, read `CHALLENGES.md`. Highlights: a safety argument in my own spec that
was **wrong** and destroyed live data under load; a stale cached file size that made the
collector delete a full container; 32 bytes of every recipe sitting outside the checksum;
two optimizations that measured as completely worthless; and three benchmarks that were
measuring themselves.

---

## Build and test

```bash
./scripts/dev.sh ./scripts/check.sh        # all three configurations
./scripts/dev.sh ./scripts/check.sh none   # just the fast one
```

Three configurations, because ASan and TSan cannot coexist in one binary:

| | catches |
|---|---|
| normal `-O2` | the code that is actually shipped and benchmarked |
| ASan + UBSan | memory safety, undefined behaviour (S5) |
| TSan | data races, lock-order inversions (S6) |

Randomized tests are seeded and print the seed:
`DEDUPE_SEED=0x... ./build-none/test_gc_stress` replays a failure exactly.

```bash
./scripts/dev.sh ./build-none/bench_corpus    # the storage-reduction numbers
./scripts/dev.sh ./build-none/bench_gc        # GC vs foreground latency
./scripts/dev.sh ./build-none/crashtest 20    # exhaustive crash sweep (slow)
```

---

## Status

| Task | | |
|---|---|---|
| T0 | Foundations: container, CMake+CTest, harness, spec, journal | ✅ |
| T1 | SHA-256 + CRC32C, runtime-dispatched hardware backends | ✅ |
| T2 | FastCDC content-defined chunker | ✅ |
| T3 | On-disk formats + checked, fault-injectable I/O layer | ✅ |
| T4 | Sharded Robin Hood fingerprint index | ✅ |
| T5 | Store: ingest, restore, verify, CLI | ✅ |
| T6 | Concurrency: multi-threaded ingest | ✅ |
| T7 | Background space reclamation | ✅ |
| T8 | Crash & fault injection | ✅ |
| T9 | Benchmarks — the storage-reduction numbers | ✅ |
| T10 | Hardening, docs, résumé reconciliation | ✅ |

## What this deliberately is not

No compression (so the reduction is attributable to deduplication alone), no FUSE
filesystem, no multi-node replication, no on-disk index tier. Each omission and its
reasoning is in [`docs/SPEC.md`](docs/SPEC.md) §1, and the resulting limits — including
**process-crash consistency is tested, power-loss durability is not** — are stated in §8
and in [`docs/RESUME.md`](docs/RESUME.md) rather than left for a reader to discover.
