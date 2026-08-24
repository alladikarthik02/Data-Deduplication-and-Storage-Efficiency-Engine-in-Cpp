# `dedupe` — Data Deduplication and Storage Efficiency Engine (C++20)

A single-node, content-addressed deduplicating storage engine. Files are split into
**content-defined chunks**, fingerprinted with SHA-256, and only chunks not already
present are stored. A **background space-reclamation** thread frees chunks no file
references any more, while ingests and restores keep running.

- **Spec:** [`docs/SPEC.md`](docs/SPEC.md) — architecture, essential functions, safety
  requirements (S1–S14), task breakdown.
- **Bug journal:** [`docs/CHALLENGES.md`](docs/CHALLENGES.md) — every bug hit while
  building this, with the reasoning, including the wrong hypotheses.
- **Measurements:** [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) — every number, with the
  command that produced it.

## Build and test

Everything builds and runs **inside Linux in Docker** — the correctness claims here are
about `fsync` ordering, atomic `rename`, torn-tail recovery and `flock`, all of which
behave differently on macOS/APFS. See [`docs/SPEC.md`](docs/SPEC.md) §8.6 for the limits
of what this environment can honestly prove.

```bash
./scripts/dev.sh ./scripts/check.sh     # build + test in all 3 configurations
```

That runs the full suite three times: normal `-O2`, ASan+UBSan (memory safety, S5), and
TSan (data races, S6). For a single configuration:

```bash
./scripts/dev.sh ./scripts/check.sh none
```

Randomized tests are seeded and print the seed; replay a failure with
`DEDUPE_SEED=0x... ./build-none/test_roundtrip`.

## Status

| Task | What | State |
|---|---|---|
| T0 | Foundations: container, CMake+CTest, test harness, spec, bug journal | ✅ done |
| T1 | SHA-256 + CRC32C fingerprinting, runtime-dispatched hardware backends | ✅ done |
| T2 | FastCDC content-defined chunker | ✅ done |
| T3 | On-disk formats + checked/injectable I/O layer | ✅ done |
| T4 | Sharded memory-efficient fingerprint index | — |
| T5 | Store: ingest + restore + CLI | — |
| T6 | Concurrency: multi-threaded ingest | — |
| T7 | Background space reclamation (GC) | — |
| T8 | Crash & fault injection | — |
| T9 | Benchmarks — the résumé's numbers | — |
| T10 | Hardening, docs, résumé reconciliation | — |
