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
