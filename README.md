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

## Contents

- [Building and running](#building-and-running)
- [Using the command line](#using-the-command-line)
- [Using the library](#using-the-library)
- [Architecture](#architecture)
  - [Components](#components)
  - [What the store looks like on disk](#what-the-store-looks-like-on-disk)
  - [The ingest path](#the-ingest-path)
  - [The restore path](#the-restore-path)
  - [Threads](#threads)
- [Design decisions](#design-decisions)
- [Measurements](#measurements)
- [Tests](#tests)
- [Repository layout](#repository-layout)
- [Documentation](#documentation)
- [Limitations](#limitations)
- [License](#license)

## Building and running

Everything builds and runs inside Linux in a container. That is not a convenience: the
guarantees here are about `fsync` ordering, atomic `rename`, torn-tail recovery and
`flock`, and all four behave differently on macOS. Docker is the only requirement.

```bash
./scripts/dev.sh ./scripts/check.sh     # build and run every test, in all three configurations
```

`scripts/dev.sh` builds the image the first time and bind-mounts the repository, so you
edit on the host and compile on Linux. The three build directories it produces are
`build-none`, `build-address` and `build-thread`.

## Using the command line

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

## Using the library

The CLI is a thin wrapper over `include/dedupe/store.h`; anything it does is available to
a caller. Link against the static library the build produces:

```cmake
target_link_libraries(myapp PRIVATE dedupe_core)
target_include_directories(myapp PRIVATE ${CMAKE_SOURCE_DIR}/include)
```

Nothing throws. Fallible calls return `Status` (ok, or an error code and message) or
`Result<T>` (a `Status` plus a value), both marked `[[nodiscard]]`, so ignoring a failed
`fsync` is a compile error rather than a silent data loss bug. `RETURN_IF_ERROR` and
`ASSIGN_OR_RETURN` keep the call sites readable.

```cpp
#include "dedupe/store.h"

using namespace dedupe;

auto opened = Store::open("/var/lib/backups");        // Result<std::unique_ptr<Store>>
if (!opened.ok()) {
  std::fprintf(stderr, "open: %s\n", opened.status().message().c_str());
  return 1;
}
Store& store = **opened;

auto in = store.ingest_file("nightly/report.pdf", "/data/report.pdf");
if (!in.ok()) { /* in.status().code() is an Err enum */ }
printf("%llu chunks, %llu new, %llu deduplicated\n",
       in.value().chunks_total, in.value().chunks_new, in.value().chunks_deduped);

RETURN_IF_ERROR(store.restore("nightly/report.pdf", "/tmp/report.pdf"));
ASSIGN_OR_RETURN(std::vector<uint8_t> bytes,
                 store.restore_bytes("nightly/report.pdf"));   // or straight to memory
```

Opening a store takes an exclusive `flock` for as long as the handle lives, recovers any
interrupted compaction, and rebuilds the index by scanning the containers. The rest of the
surface is `remove`, `exists`, `list`, `stats`, `read_recipe` and `verify`:

```cpp
ASSIGN_OR_RETURN(StoreStats s, store.stats());
printf("%.2fx across %llu files\n", s.dedup_ratio(), s.files);

ASSIGN_OR_RETURN(VerifyReport v, store.verify(VerifyLevel::kDeep));   // kQuick skips re-reading chunks
for (const std::string& problem : v.problems) std::fputs(problem.c_str(), stderr);
```

Ingesting many files in one go is worth a session. The durability cost of publishing a
file is one `fsync` per container it touched, and a session lets consecutive files share
an open container instead of rolling a new one each time:

```cpp
ASSIGN_OR_RETURN(IngestSession session, store.begin_ingest());
for (const auto& [name, path] : files) {
  RETURN_IF_ERROR(session.ingest_file(name, path).status());
}
RETURN_IF_ERROR(session.finish());    // flush and close the session's container
```

Reclaiming space is a separate object, so the caller decides whether it runs inline or on
its own thread:

```cpp
#include "dedupe/gc.h"

Gc gc(&store);
ASSIGN_OR_RETURN(GcStats g, gc.run_once());        // one deterministic cycle
printf("swept %llu entries, deleted %llu containers\n", g.swept, g.containers_deleted);

gc.start();                                        // or: a background thread, every 2s
// ...
gc.stop();
```

`Store` is safe to use from several threads at once: concurrent `ingest_file`, `restore`
and `stats` are expected, and the concurrency suites exercise exactly that under TSan.
Give each thread its own `IngestSession`, and run one collector per store.

Store behaviour is set at open time through `StoreOptions`, which carries the chunker
parameters (minimum, average and maximum chunk size, and the two masks) and
`verify_fingerprint_on_read`, on by default. Chunker parameters are recorded in the
store's superblock on creation and reloaded from it afterwards, because re-chunking an
existing store with different parameters would silently stop it deduplicating against
everything already there.

## Architecture

### Components

| | |
|---|---|
| `chunker.h` | Gear rolling hash and the FastCDC cut-point rules. Pure and stateless, plus a streaming wrapper that cuts across buffer boundaries without copying the whole input |
| `sha256.h`, `crc32c.h` | Fingerprints and record checksums. Each has a portable implementation and a hardware one selected at runtime, with the two tested against each other |
| `index.h` | Fingerprint to location. 256 independently locked shards, each a flat Robin Hood table of 32-byte entries |
| `format.h` | The on-disk structures: container header, chunk record header, recipe. Encode, decode, and the validation that decoding a hostile file requires |
| `container.h` | Append-only chunk files. The writer buffers and tracks what is visible against what is durable; the reader verifies; the scanner recovers a torn tail |
| `io.h` | Every syscall the engine makes, wrapped so it returns a `Status`, plus atomic file publication and the fault injection the crash tests drive |
| `store.h` | The engine: open and recover, ingest, restore, verify, and the durability ordering that ties them together |
| `gc.h`, `gc_journal.h` | Epoch mark and sweep, copy-forward compaction, and the intent log that makes compaction crash-safe |

### What the store looks like on disk

```
<store>/
  STORE              superblock: magic, version, store UUID, chunker parameters, CRC32C
  LOCK               flock'd for as long as a process holds the store open
  containers/
    c00000001.dat    32-byte header, then chunk records appended forever
    c00000002.dat
  recipes/
    <sha256(name)>.rcp
  gc-journal         compaction intent log: BEGIN, COMMIT, DONE, DELETE
```

A chunk record is a 48-byte header followed by the payload:

```
offset  size  field
     0     4  "CHNK", the magic that marks a record boundary
     4     4  payload length
     8     4  CRC32C of the payload
    12     4  flags
    16    32  SHA-256 fingerprint of the payload
    48     n  payload
```

Carrying the full fingerprint and the length in every record is what makes a container
self-describing, and therefore what makes the index disposable. A recipe is a 64-byte
header (magic, version, logical size, chunk count, the whole file's SHA-256, the length of
the original name, and a CRC32C covering all of it) followed by the name and then
`chunk_count` entries of 36 bytes each: a 32-byte fingerprint and a 4-byte length.
Recipes are named by the SHA-256 of the file name, so no user-controlled byte ever reaches
the filesystem.

### The ingest path

1. The chunker scans the incoming bytes and returns a cut point.
2. SHA-256 over that chunk gives its fingerprint.
3. The index is probed. A hit returns a location and nothing is written.
4. A miss appends a record to this session's container and inserts the location.
5. The fingerprint is appended to the recipe, which is held in memory, and the whole-file
   hash is updated. Steps 1 to 5 repeat to the end of the file.
6. Every container the recipe references is fsynced, including containers other threads
   wrote that this file deduplicated against.
7. The recipe is written to a uniquely named temporary file, fsynced, renamed into place,
   and the directory is fsynced.

Step 6 before step 7 is the whole durability argument. A recipe is what makes a file
exist, so it is published last, and a crash can then leave missing chunks or a missing
file but never a file that lists and cannot be read.

### The restore path

1. Read the recipe and check its CRC32C.
2. For each fingerprint in order, look up the location in the index.
3. Read the record, verify the CRC32C, and verify that the header's full 32-byte
   fingerprint is the one that was asked for.
4. Append the payload to the output.
5. Hash the reassembled file and compare against the whole-file digest in the recipe,
   which catches the one failure per-chunk checks cannot: correct chunks in the wrong
   order.

### Threads

Each ingest session owns a container, so appends never queue behind another thread. The
index is sharded 256 ways by the first byte of the fingerprint, and lookups take a shared
lock, so readers do not exclude each other. Recipes are built in thread-local memory and
touch the filesystem exactly once, so they need no lock at all, and the atomicity of
`rename` settles the case where two threads publish the same name.

The collector never holds an index lock across I/O: it marks under a shared lock, sweeps
in bounded batches that release the lock between them, and copies chunks during compaction
with no lock held. Measured against an idle store, the foreground stays within 20% at p50,
p90 and p99.

## Design decisions

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

**Containers and recipes on disk are the only authoritative state.** Because each record
describes itself, the in-memory index is a cache that is rebuilt by scanning containers
whenever the store opens. Losing it costs startup time, not data, which turns index
durability from a correctness problem into a performance one.

**Durability is an ordering rule:** never publish a reference before the data it points at
is on the disk. It appears three times over, at different scales. Chunks are fsynced
before the recipe that names them. A recipe's bytes are fsynced before the rename that
publishes them, and the directory is fsynced after it. The collector's journal record
reaches the disk before the compaction step it describes. Recovery truncates any torn tail
at the first record that fails its checks, which is safe precisely because a published
recipe can only reference bytes that were already fsynced.

**Space is reclaimed by mark and sweep, not reference counts.** Counts race with in-flight
deduplication hits, and they cannot heal: one lost decrement leaks a chunk forever and
nothing ever notices. Instead the collector walks every recipe, marks what it finds, and
removes index entries that are both unmarked and untouched since before the oldest ingest
still running. Containers that fall below half live are rewritten and the originals
deleted, with the intent written to a journal first so an interrupted compaction is either
finished or undone on the next open.

**The index is a flat open-addressed table rather than a node-based map.** At 32 bytes per
entry, two fit in a cache line and a lookup is usually one memory touch instead of a
pointer chase. Robin Hood probing keeps displaced entries near their home slot, which
allows the early exit that makes unsuccessful lookups cheap, and unsuccessful is the
common case here because every new chunk misses. Deletion shifts the following run back
rather than leaving tombstones, which would lengthen every probe until a rebuild.

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

## Repository layout

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
