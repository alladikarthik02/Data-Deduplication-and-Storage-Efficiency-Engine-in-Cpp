# dedupe

A deduplicating storage engine for backup data, written in C++20 with no external
dependencies. The chunker, SHA-256, CRC32C, hash index, on-disk formats, crash recovery
and garbage collector are all implemented in this repository.

It splits each file into content-defined chunks, names every chunk by the SHA-256 of its
bytes, and writes a chunk only if the store has not seen it before. A stored file becomes
a *recipe*: an ordered list of fingerprints and nothing else. A background collector
reclaims chunks that no recipe references any more, while ingest and restore keep running.

On eight successive backups of a 2,000-file tree it held 3,766 MiB of logical data in
511 MiB on disk, a 7.36x reduction. Run against unique random data instead, the same
engine reports 0.99x, which is the control that shows the ratio comes from real redundancy
in the corpus rather than from the benchmark.

## Building and running

Everything builds and runs inside Linux in a container. That is not a convenience: the
guarantees here are about `fsync` ordering, atomic `rename`, torn-tail recovery and
`flock`, and all four behave differently on macOS. Docker is the only requirement.

```bash
./scripts/dev.sh ./scripts/check.sh     # build and run every test, in all three configurations
```

`scripts/dev.sh` builds the image the first time and bind-mounts the repository, so you
edit on the host and compile on Linux.

```bash
./scripts/dev.sh bash -c '
  S=/tmp/store
  ./build-none/dedupe --store $S ingest myfile.bin
  ./build-none/dedupe --store $S stats
  ./build-none/dedupe --store $S verify --deep
'
```

The command surface is small:

```
dedupe --store <dir> ingest <file> [name]    add a file
dedupe --store <dir> restore <name> <out>    rebuild it, byte for byte
dedupe --store <dir> cat <name>              the same, to stdout
dedupe --store <dir> rm <name>               delete (space returns at the next gc)
dedupe --store <dir> gc [--daemon <secs>]    reclaim space
dedupe --store <dir> ls | stats | verify [--deep]
```

## How it works

**Chunk boundaries come from the data, not from the offset.** Cutting every 8 KiB would
mean that inserting a single byte near the start of a file shifts every later boundary, so
every chunk looks new and the whole file is stored again. Instead a Gear rolling hash runs
over the bytes and a cut is made where the hash matches a mask, which makes boundaries
follow content: after a one-byte insertion, 99.9% of boundaries survive, against 0.0% for
a fixed-size control. Chunks average 9.3 KiB, with a 2 KiB floor and a 64 KiB ceiling that
only data with no variety (a long run of zeros, say) ever reaches.

**Chunks are named by SHA-256 of their contents.** Deduplication is then one index lookup.
A cryptographic hash is not overkill here: if two different chunks ever shared a
fingerprint, the second would silently never be stored, so collision resistance is a
correctness requirement rather than a nicety. The index keeps only the first 128 bits of
each fingerprint to halve its memory, and every read verifies the full 32 bytes from the
chunk's own header, so a truncated-key collision would fail loudly instead of returning
the wrong bytes.

**Containers and recipes on disk are the only authoritative state.** Chunks are appended
to container files as self-describing records: a 48-byte header carrying the length, a
CRC32C, and the full fingerprint, followed by the payload. Because each record describes
itself, the in-memory index is a cache that is rebuilt by scanning containers whenever the
store opens. Losing it costs startup time, not data, which turns index durability from a
correctness problem into a performance one.

**Durability is an ordering rule.** Never publish a reference before the data it points at
is on the disk. Publishing a file therefore looks like this:

```
append chunks to containers
fsync every container this recipe references   (including ones other threads wrote)
write the recipe to a temporary file
fsync it
rename it into place                           (atomic: the file appears whole or not at all)
fsync the directory                            (so the name itself is durable)
```

A crash can leave four states, and only one of them is unacceptable: a recipe on disk
pointing at chunks that never reached it, which is a file that lists but cannot be read.
The ordering above makes that state unreachable. Recovery truncates any torn tail at the
first record that fails its checks, which is safe precisely because a published recipe can
only reference bytes that were already fsynced.

**Space is reclaimed by mark and sweep, not reference counts.** Counts race with in-flight
deduplication hits, and they cannot heal: one lost decrement leaks a chunk forever and
nothing ever notices. Instead the collector walks every recipe, marks what it finds, and
removes index entries that are both unmarked and untouched since before the oldest ingest
still running. Containers that fall below half live are rewritten and the originals
deleted, with the intent written to a journal first so an interrupted compaction is either
finished or undone on the next open.

**Threads are kept apart rather than serialized.** Each ingest writes to its own container,
and the index is split into 256 independently locked shards chosen by the first byte of the
fingerprint, with lookups taking a shared lock. The cost is restore locality, since one
file's chunks end up spread across containers.

## Measurements

Every number below is produced by a command recorded in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

| | |
|---|---|
| Storage reduction, 8 backups of a 2,000-file tree, 3% churn | 7.36x |
| Same engine, unique random data (control) | 0.99x |
| Boundary preservation after a 1-byte insert, content-defined vs fixed | 99.9% vs 0.0% |
| Index memory, 1M fingerprints | 51.5 B/entry, against 92.7 for a tuned `unordered_map` |
| Ingest throughput, 1 thread to 8 | 275 to 893 MB/s |
| SHA-256, portable and hardware-dispatched | 124 and 2,074 MB/s |
| CRC32C, portable and hardware-dispatched | 2,085 and 8,056 MB/s |
| Crash injection | 225 points, 52 real process kills, 0 failures |
| Collector running against idle, p50/p90/p99 | within 20% |

The reduction is set by how many times you back up rather than by how much changes
between backups: raising churn from 1% to 25% moved the ratio only from 7.14x to 6.84x,
while going from 4 generations to 12 moved it from 3.86x to 10.69x. That is what
content-defined chunking buys, since each edit is confined to the chunk containing it.

Ingest does not scale linearly with threads because it is bound by `fsync`, not by lock
contention. Batching container writes cut write syscalls 88-fold and changed throughput by
nothing measurable, which is the evidence for that claim rather than an assertion.

## Tests

```bash
./scripts/dev.sh ./scripts/check.sh          # all three configurations
./scripts/dev.sh ./scripts/check.sh none     # just the fast one
```

103 tests in 13 suites, each run normally, under ASan with UBSan, and under TSan, which is
309 runs per gate. Three builds rather than one because ASan and TSan cannot share a
binary. Randomized tests print their seed, so a failure replays exactly:

```bash
DEDUPE_SEED=0x... ./build-none/test_gc_stress
```

Benchmarks and the crash sweep are slow and deliberately kept out of the gate:

```bash
./scripts/dev.sh ./build-none/bench_corpus    # the storage-reduction figures
./scripts/dev.sh ./build-none/bench_gc        # collector against foreground latency
./scripts/dev.sh ./build-none/crashtest 20    # kill -9 at the Nth syscall, exhaustively
```

## Layout

```
include/dedupe/   public headers: chunker, sha256, crc32c, index, container, format,
                  io, store, gc, gc_journal, status, types
src/              implementations of the above
tests/            13 suites, from unit tests to concurrent stress and crash recovery
bench/            chunker, hashes, index, ingest, collector, and the backup corpus
tools/            the CLI, and crashtest
scripts/          dev.sh runs anything in the container; check.sh builds and tests
docs/             SPEC, CHALLENGES, BENCHMARKS, CLAIMS
scratch/          the ARMv8 SHA-2 spike that preceded the hardware backend
```

## Documentation

[docs/SPEC.md](docs/SPEC.md) is the design document: architecture, the required function
surface, safety requirements S1 to S14 with the test that proves each one, the task
breakdown the project was built in, and the holes found by attacking the design.

[docs/CHALLENGES.md](docs/CHALLENGES.md) is the engineering journal, 17 defects written up
with the reasoning that found them, including the first diagnoses that were wrong. It is
the one worth reading. Among them: a safety argument in the spec that was itself wrong and
destroyed live data under concurrent load, a stale cached file size that made the collector
delete a container full of live chunks, 32 bytes of every recipe sitting outside its
checksum, two optimizations that measured as worthless, and three benchmarks that were
measuring themselves.

[docs/BENCHMARKS.md](docs/BENCHMARKS.md) has every measurement with the command that
produced it. [docs/CLAIMS.md](docs/CLAIMS.md) checks each headline claim against what was
actually measured, and rewrites the ones the numbers did not support.

## Limitations

The crash tests kill the process, so the operating system's page cache survives them. That
is crash consistency, not power-loss durability, and proving the latter needs hardware.

The index lives in RAM, roughly 5 to 6 GB per TB of unique data, which is the real ceiling
on how much one machine can hold. A production system would tier it to disk behind a
filter and a locality-preserving cache.

Chunk payloads are not compressed, deliberately, so that the storage reduction is
attributable to deduplication alone. The collector reads every recipe each cycle, so its
cost scales with total data rather than with garbage. There is no FUSE filesystem, no
multi-node replication, and no on-disk index tier.

## License

BSD 3-Clause. See [LICENSE](LICENSE).
