# `dedupe` — measurements

Every number here was produced by a command written next to it, inside the project
container, on the platform recorded below. An optimization without a measurement does
not go into this project (SPEC R3.3).

**Platform (constant for all rows unless stated):** Docker `dedupe-dev` — Ubuntu 24.04,
GCC 13.3, **linux/aarch64** (Apple silicon), 8 CPUs, 8 GiB. Build type
`RelWithDebInfo` (`-O2 -g`), no sanitizers.

---

## T1 — fingerprint and integrity primitives

```bash
./scripts/dev.sh ./build-none/bench_hash
```

64 MiB random buffer (far past any cache), best of 5 runs.

| Primitive | Throughput | Notes |
|---|---:|---|
| `sha256` — portable scalar | **123.9 MB/s** | The reference path. What a CPU without the ARMv8 SHA-2 extension runs. |
| `sha256` — ARMv8 SHA-2 | **2073.9 MB/s** | **16.7× faster.** Runtime-dispatched via `AT_HWCAP`. |
| `crc32c` — slicing-by-8 table | **2084.7 MB/s** | Portable path, 8 KiB of compile-time tables. |
| `crc32c` — ARMv8 CRC32 | **8055.5 MB/s** | **3.9× faster** than the table. Runtime-dispatched. |
| `crc32c` — bitwise | 92.1 MB/s | The polynomial definition. Test oracle only, never production. |

### What these numbers mean for the engine

- **SHA-256 was the ceiling on the whole ingest path.** At 123.9 MB/s, no amount of
  chunker or index tuning could have pushed ingest past ~124 MB/s — every ingested byte
  must be fingerprinted. The hardware backend raises that ceiling to ~2.0 GB/s, which
  moves the bottleneck to somewhere worth optimizing (I/O and the index).
- **CRC32C is effectively free** relative to SHA-256 (4× faster even in software, 65×
  in hardware). That is what makes the design in SPEC 3.3 affordable: every chunk read
  can verify its CRC without measurably slowing restore. Verifying with SHA-256 instead
  would have cost ~17× more on the read path for no additional protection against the
  thing a CRC is for.
- **Best-of-5, not mean**, on purpose: every source of noise (scheduler, co-tenant
  container, page faults) can only make a sample slower, so the minimum time is the
  least-biased estimate of the machine's capability.

### Honesty notes

- This is a VM on Apple silicon, so absolute numbers are specific to this host. The
  *ratios* (hardware vs software for the same algorithm, on the same run) are the
  durable result.
- The benchmark hashes a buffer already resident in RAM. Real ingest also reads from
  disk, so the end-to-end number will be lower — measured for real in T9, not
  extrapolated from here.

---

## T2 — content-defined chunking

```bash
./scripts/dev.sh ./build-none/bench_chunker
./scripts/dev.sh ./build-none/test_chunker      # prints the distribution + boundary-shift table
```

64 MiB random buffer, best of 5. Parameters: Min 2 KiB / Avg 8 KiB / Max 64 KiB,
normalized chunking level 2.

| Path | Throughput | |
|---|---:|---|
| `find_cut_point` — raw scan, zero copy | **1884.5 MB/s** | The algorithm's ceiling. |
| `Chunker::feed` — 1 MiB reads, naive copy | 1352.6 MB/s | Copies every byte into a carry buffer: **−28%**. |
| `Chunker::feed` — splice-region copy | **1805.9 MB/s** | **+33.5%.** Within 4% of the ceiling. |
| chunk + `sha256` — full ingest CPU | 808.6 → **966.1 MB/s** | **+19.5%** end-to-end from the same change. |

The optimization: copy only the *splice region* (the carried partial chunk plus MaxSize
bytes of new data — enough to resolve the chunk straddling the read boundary) and chunk
the rest of the caller's buffer in place. Per 1 MiB read that is ~128 KiB copied instead
of 1 MiB. Chunk boundaries are bit-identical before and after (7221 chunks both ways).

### Chunk-size distribution (SPEC §8.8 — measured, not assumed)

8 MiB of random data, 905 chunks:

| mean | p1 | p50 | p99 | max | at MaxSize | at MinSize |
|---:|---:|---:|---:|---:|---:|---:|
| **9267 B** | 2540 | 9235 | 16040 | 22788 | 0.00% | 0.00% |

**The mean is 9.3 KiB, not the 8 KiB the parameter is named after** — and that is
correct behaviour, not a bug. Derivation, treating each mask as a per-byte cut
probability:

```
E[len] = MinSize                                  (skipped, never tested)
       + E[min(Exp(1/2^15), AvgSize − MinSize)]   (reluctant region, 15-bit mask)
       + P(no cut there) × 2^11                   (eager region, 11-bit mask)
       = 2048 + 5602.4 + 1697.9  =  9348.2 B
```

Predicted 9348.2 B vs measured 9294 B (64 MiB) — **0.58% error**. See `CHALLENGES.md` B5.

The same model says `P(a chunk is forced out at MaxSize) = 5.7e-13` on random data, ~1
in 1.7 × 10¹², which is why `at_max` is 0.00%. MaxSize is not a size control on real
data; it is a **bound for pathological input**, and the all-zeros test confirms it does
its job there (a 1 MiB run of one byte gives the gear hash no entropy, so all 16 chunks
come out at exactly 64 KiB).

### Boundary-shift preservation — the R1.1 result

8 MiB corpus, one byte inserted, measuring what fraction of chunk *fingerprints* survive:

| Edit | Content-defined | Fixed-size 8 KiB |
|---|---:|---:|
| insert 1 byte at offset 0 | **99.9%** | **0.0%** |
| insert 1 byte at 25% | 99.9% | 25.0% |
| insert 1 byte at 50% | 99.8% | 50.0% |
| 20 scattered edits in 4 MiB | 95.3% | — |

The fixed-size column is the boundary-shift problem quantified: an insertion at offset 0
destroys **every** block, and an insertion at 25% destroys exactly the 75% of blocks that
follow it. Content-defined chunking loses ~1 chunk per edit and re-synchronizes.

---

## T4 — the fingerprint index

```bash
./scripts/dev.sh ./build-none/bench_index
```

1,000,000 fingerprints (≈8.9 GiB of logical data at the measured 9294 B mean chunk).
Memory is **resident set size**, measured in a forked child per case so the number is real
allocator behaviour rather than a theoretical byte count (see `CHALLENGES.md` B9 for why
the first attempt was wrong).

| Implementation | RSS | B/entry | insert | lookup |
|---|---:|---:|---:|---:|
| **`dedupe::Index`** (sharded, flat, Robin Hood) | **49.1 MiB** | **51.5** | 212 ns | 88 ns |
| `unordered_map<Digest32, Location>` | 88.4 MiB | 92.7 | 235 ns | 45 ns |
| `unordered_map<std::string, Location>` | 134.2 MiB | 140.7 | 435 ns | 261 ns |

**R3.1: 1.80× smaller** than a *tuned* `unordered_map` (32-byte digest stored inline, no
per-key heap allocation — the strongest reasonable baseline, not a straw man), and
**2.73× smaller** than the version most people would actually write. At 1M entries the
table runs at load 0.706 with a **maximum probe distance of 19**.

Two things stated plainly rather than buried:

- **Our lookup is ~2× slower than the tuned baseline** (88 ns vs 45 ns). Most of that gap
  is the per-shard `shared_mutex`, which the baseline does not have at all — an
  unsynchronized map is not a usable comparison for a concurrent engine, but it is the
  honest number and it is what the table shows.
- **It does not matter**, and the last column of the scaling table is the proof rather
  than the assertion.

### R3.2 — does deduplication throughput hold as the store grows?

| entries | insert | lookup | RSS | B/entry | max probe | **dedup MB/s** |
|---:|---:|---:|---:|---:|---:|---:|
| 62 500 | 99 ns | 47 ns | 3.8 MiB | 63.4 | 12 | **2021.4** |
| 125 000 | 104 ns | 48 ns | 7.4 MiB | 62.3 | 15 | **2010.9** |
| 250 000 | 116 ns | 92 ns | 13.5 MiB | 56.5 | 34 | **1977.9** |
| 500 000 | 126 ns | 147 ns | 22.2 MiB | 46.6 | 28 | **1969.3** |
| 1 000 000 | 168 ns | 257 ns | 49.1 MiB | 51.5 | 19 | **1924.7** |

Index lookup latency grows **5.4×** over a 16× dataset increase — and that is not an
algorithmic problem, it is the memory hierarchy: at 62 500 entries the table is 3.8 MiB
and lives in cache; at 1M it is 49 MiB and every probe is a main-memory access. The probe
distance does not grow (12 → 19), which is what says the *algorithm* is fine.

Deduplication throughput over that same 16× growth: **2021 → 1925 MB/s, a 4.8% decline.**
That is the number résumé bullet R3.2 is about, and it holds because a chunk costs ~4.5 µs
to fingerprint and ~0.1 µs to look up — the index is under 2% of the work. Reporting the
index latency alone would have overstated the problem; reporting only the throughput would
have hidden it. Both are here.
