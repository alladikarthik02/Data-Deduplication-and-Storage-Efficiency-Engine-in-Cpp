# `dedupe` — bug journal & engineering challenges

Every bug hit while building this engine, written down with the *reasoning*, not just the fix.
Small ones are included on purpose — a missing tool sits next to a design-breaking discovery —
because the point is that reading this cold should make each failure legible, and defensible
out loud.

**Format per entry:** Symptom → Hypotheses (including the wrong ones) → How I isolated it →
Root cause → Fix → Generalizes to.

Wrong hypotheses are kept deliberately. Disproving your own first guess is the most defensible
story there is; a journal that only records correct conclusions hides the part of the work that
was actually hard.

**Legend:** 🧱 environment/tooling · 🐛 code defect · 🔬 design-invalidating discovery ·
⚔️ concurrency/timing · 💾 durability/crash-consistency

---

## B1 🧱 Cannot read the résumé PDF: no `pdftotext`, no `pdftoppm`

**Symptom.** The very first action of the project — read the résumé to find out what project #1
actually claims — failed twice:

```
pdftotext not found
pdftoppm is not installed. Install poppler-utils ...
```

**Hypotheses.**
- (H1) The PDF is malformed or encrypted. — *Rejected immediately:* the failure names a **missing
  local binary**, not a parse error. Nothing had opened the file yet.
- (H2) No PDF tooling exists on this machine at all. — Partly right, and the useful direction.

**How I isolated it.** `which pdftotext mutool qpdf python3` — the first three missing, `python3`
present via pyenv. Then `python3 -c "import pypdf"` succeeded on the first try, while `PyPDF2`,
`pdfminer` and `fitz` all raised `ModuleNotFoundError`.

**Root cause.** Poppler (which ships `pdftotext`/`pdftoppm`) is not installed on this macOS host,
and the built-in PDF read path depends on it for rendering. The *only* PDF capability present was a
Python library that happened to already be in the pyenv environment.

**Fix.** Extract the text with `pypdf` directly instead of installing system packages:

```bash
python3 -c "from pypdf import PdfReader; print(PdfReader('résumé.pdf').pages[0].extract_text())"
```

Deliberately did **not** `brew install poppler` — installing a system package to read one file
changes the user's machine for a one-shot need, and a capability already present is strictly
cheaper than one that has to be installed.

**Generalizes to.** When a tool is missing, first enumerate what you *do* have before installing
anything. And read the error's grammar: "command not found" is an availability problem, never a
correctness problem with your input.

---

## B2 🧱 Docker daemon not running: `dial unix .../docker.sock: no such file or directory`

**Symptom.** Verifying the build environment before committing the spec to it:

```
failed to connect to the docker API at unix:///Users/…/.docker/run/docker.sock;
check if the path is correct and if the daemon is running
```

**Hypotheses.**
- (H1) Docker isn't installed. — *Rejected:* the **client** answered and produced a formatted error,
  which means the `docker` CLI binary exists and parsed the command. Only the *server* half is
  unreachable.
- (H2) This is the `docker-credential-desktop` PATH problem from the previous project. — *Rejected:*
  that one fails during registry auth with an `exec: executable file not found` message. This one
  fails at socket connect, strictly earlier in the sequence. Different layer.
- (H3) Docker Desktop simply isn't running, so nothing is listening on the socket. — Most likely: the
  socket **path** is well-formed and user-scoped; the file just doesn't exist, which is exactly what
  a stopped daemon looks like.

**How I isolated it.** The error names the socket path and says the file does not exist — not
"permission denied" (which would mean a running daemon with wrong ownership) and not "connection
refused" (which would mean something bound and then rejected us). *No such file* is the specific
signature of "nothing ever created this socket."

**Fix.** `open -a Docker` to start Docker Desktop, then continue writing documentation while the VM
boots — the daemon takes ~30 s, and that time overlaps with work that doesn't need it.

**Generalizes to.** Client/server tools fail in layers, and the exact errno tells you which layer:
*no such file* = never started · *connection refused* = started, then rejected · *permission denied*
= running, wrong ownership · *timeout* = alive but wedged. Reading the errno saves you from
debugging the wrong half. Also: verify the environment **before** writing a spec that assumes it.

---

*(Entries continue as the build proceeds. Numbering is stable — entries are never renumbered,
so references from `SPEC.md`, commit messages and the README stay valid.)*

---

## B3 🔬 The CPU has CRC32 instructions, but the compiler refuses to admit it

**Symptom.** Planning T1's CRC32C, the obvious approach was `<arm_acle.h>` and the
`__crc32c*` intrinsics. Those are only declared when `__ARM_FEATURE_CRC32` is defined.
A probe said:

```
__ARM_FEATURE_CRC32 defined at default -march: NO
AT_HWCAP=0xefb3ffff  HWCAP_CRC32 bit set: YES
```

The compiler says the feature is unavailable. The kernel says the CPU has it. Both are
telling the truth about different things.

**Hypotheses.**
- (H1) The CPU genuinely lacks the extension and `HWCAP` is lying. — *Rejected:* HWCAP
  is populated by the kernel from the CPU's own ID registers; it is the most
  authoritative source available in userspace.
- (H2) Ubuntu's GCC is misconfigured. — *Rejected:* this is stock behaviour. GCC targets
  whatever `-march` says, and Ubuntu's default is baseline `armv8-a`, in which CRC32 is
  an **optional** extension (it only became mandatory in ARMv8.1).
- (H3) The macro describes the **compilation target**, not the **execution host**. —
  Correct, and the whole point.

**Root cause.** `__ARM_FEATURE_CRC32` answers "may I emit these instructions
unconditionally?", not "does this machine have them?". Building with
`-march=armv8-a+crc` would make the macro appear — and would license GCC to emit CRC
instructions **anywhere in the binary**, including in code that runs before any feature
check. On a CPU without the extension, that binary dies with `SIGILL`.

**Fix.** Runtime dispatch, verified by a compile probe first:

1. `__attribute__((target("+crc")))` on the hardware function **only** — that one
   function is compiled with the extension, the rest of the translation unit stays
   baseline.
2. `getauxval(AT_HWCAP) & HWCAP_CRC32` at runtime, cached in a function-local static,
   to choose between the hardware and table implementations.

One portable binary, full speed where available. The same pattern was then reused for
SHA-256 in B4.

**The safety consequence, which is the part that actually matters.** Two
implementations of the same checksum is a *correctness* hazard, not just a performance
feature: if the hardware and software paths disagreed by one bit, a store written on a
CRC-capable machine would read back as corrupt on one without — data loss that only
appears when you move the disk. So `tests/test_crc32c.cpp` differentially tests all
three implementations (bitwise oracle, table, hardware) against each other across every
length 0–300 and at six unaligned offsets, not just against a single published vector.

**Generalizes to.** Compile-time feature macros describe the *target*, runtime feature
bits describe the *host*, and conflating them produces binaries that work on the build
machine and `SIGILL` in production. And: whenever you add a second implementation of
anything that gets **persisted**, the differential test between implementations is
mandatory, because a divergence is silent until it is a data-loss incident.

---

## B4 🔬 SHA-256 was the ceiling on the entire engine — 123.9 MB/s

**Symptom.** Not a failure; a measurement. The first `bench_hash` run:

```
sha256                        115.2 MB/s
crc32c (table, sw)           2081.3 MB/s
crc32c (hardware)            8070.5 MB/s
```

SHA-256 was **70× slower** than the integrity checksum sitting next to it.

**Why that is a problem and not a curiosity.** Every ingested byte must be fingerprinted
— it is unavoidable work on the critical path. So 123.9 MB/s was a hard ceiling on
ingest throughput for the whole engine, and *no* amount of chunker or index tuning
(SPEC R3.3) could have moved it. Tuning anything else first would have been optimizing a
component that was never the bottleneck.

**Hypotheses for the fix.**
- (H1) My scalar implementation is unusually slow; unroll the rounds and use a rolling
  16-word message schedule. — Plausible: ~30 cycles/byte vs the ~12–15 a good scalar
  implementation achieves. Worth maybe 2×. *Kept as a fallback, not pursued first.*
- (H2) Use the ARMv8 SHA-2 extension (`sha256h`, `sha256h2`, `sha256su0`, `sha256su1`)
  with the same runtime dispatch built in B3. Worth ~15×. — Chosen: an order of
  magnitude beats a factor of two, and the dispatch machinery already existed.

**How I de-risked it before integrating.** Wrote a standalone spike
(`scratch/spike_sha_hw.cpp`) that hashes one hand-padded block of `"abc"` and compares
against the NIST digest. If the intrinsics had not been usable under the target
attribute, that would have failed in one compile instead of after an hour of
integration. It matched on the first run.

**One deliberate choice worth defending.** The 16 four-round groups follow a strict
rotating pattern over `MSG0..MSG3` and two alternating temporaries. I **generated** that
sequence from the pattern rather than typing it out, because an index typo in this
algorithm does not fail to compile — it silently produces a wrong digest. In a
content-addressed store, a wrong digest means aliasing unrelated chunks: the worst class
of bug this project can have. Hand-typing 100 lines of rotating indices is exactly the
kind of task where a human's error rate is not zero and a generator's is.

**Result.**

| | before | after | |
|---|---:|---:|---|
| SHA-256 | 123.9 MB/s | **2073.9 MB/s** | **16.7×** |

The ingest ceiling moved from ~124 MB/s to ~2.0 GB/s, which pushes the bottleneck onto
I/O and the index — the things SPEC R3.3 actually claims were tuned.

**Generalizes to.** Measure before optimizing, and measure the *whole* path: the reason
this was worth doing is not that SHA-256 was slow in isolation but that it was the
serial bottleneck for everything downstream. Also: spike a risky dependency in the
smallest possible standalone program before threading it through your codebase.

---

## Note on T1: no code defects

T1 produced two design-shaping discoveries (B3, B4) and **zero** bugs — SHA-256 matched
every NIST vector on the first run, and all three CRC implementations agreed
immediately. That is recorded rather than quietly omitted, because a bug journal that
only ever shows bugs is as misleading as one that shows none. The reason is worth
naming: both primitives had *published, independent oracles* (NIST vectors, the CRC
check value) available before a line was written, so the tests were written against an
external truth rather than against my own implementation. Later tasks — the chunker,
the index, GC — have no such oracle, which is precisely why they will need differential
and property tests instead, and why this journal should get longer from here.

---

## B5 🔬 "AvgSize = 8 KiB" produces 9.3 KiB chunks — the parameter is not the average

**Symptom.** The first distribution measurement, on 8 MiB of random data:

```
n=905  mean=9267 B  p1=2540  p50=9235  p99=16040  max=22788  at_max=0.00%  at_min=0.00%
```

The chunker is configured with `avg_size = 8192`. The mean is **13% higher**.

**Why this mattered enough to chase.** SPEC §3.4 derives everything from chunk size:
index RAM per byte of data, recipe metadata overhead, the 8 TiB capacity claim. If the
chunker's real average were unknown, every one of those numbers would be decoration.
SPEC §8.8 was written specifically to say "this is an assumption until measured" — this
is that measurement coming back different from the assumption.

**Hypotheses.**
- (H1) The masks are wrong — wrong bit counts, or I swapped strict and loose. —
  *Testable:* `popcount(mask_s)` must be 15 and `popcount(mask_l)` 11, and the strict
  one must apply *before* the average. Both checked out.
- (H2) Cut-point skipping is double-counting: `MinSize` bytes skipped *and* then added
  again. — *Rejected:* the tiling test (`chunker_is_complete_and_lossless`) proves the
  chunks sum exactly to the input length, so nothing is being counted twice.
- (H3) 8192 is not supposed to be the mean at all. — Correct.

**Root cause.** `AvgSize` in FastCDC is the point where the mask *switches*, not a target
the algorithm steers toward. The expected length is the sum of three independent pieces:

```
E[len] = MinSize                                  2048.0   (skipped, never tested)
       + E[min(Exp(1/2^15), AvgSize − MinSize)]   5602.4   (reluctant, 15-bit mask)
       + P(no cut there) × 2^11                   1697.9   (eager, 11-bit mask)
       =                                          9348.2 B
```

Measured 9294 B on 64 MiB. **0.58% error** against a model derived from first
principles — which is the strongest evidence available that the implementation is doing
exactly what the algorithm specifies, and that the surprise was in my reading of the
parameter, not in the code.

**Fix.** No code change. The *spec* was wrong, so the spec changed: §3.4's memory
arithmetic now uses the measured 9294 B. Every derived figure moved in the *favourable*
direction, which is worth stating explicitly so it does not read as motivated reasoning:

| | assumed 8192 B | measured 9294 B |
|---|---:|---:|
| index cost as % of data | 0.521% | **0.459%** |
| recipe overhead | 0.439% | **0.387%** |
| index RAM per TiB of unique data | 5.33 GiB | **4.70 GiB** |

The same model also explains a second observation: `P(chunk forced out at MaxSize)` =
5.7 × 10⁻¹³ on random data. MaxSize is therefore not a size control at all on real
input — it is a **bound for pathological input**, and the all-zeros test proves it does
that job (a 1 MiB run of a single byte gives the gear hash no entropy, no cut is ever
found, and all 16 chunks come out at exactly 64 KiB).

**Generalizes to.** When a measurement disagrees with a parameter's *name*, build the
analytic model before touching the code — if the model reproduces the measurement, the
code is right and your understanding was wrong. And: a spec figure that was flagged as
an assumption is not a liability, it is a debt you scheduled. §8.8 existed precisely so
this would be checked instead of discovered later by a capacity-planning failure.

---

## B6 🐛 My first optimization was measurably useless — the fast path never fired

**Symptom.** The chunker's streaming path measured 1352.6 MB/s against 1884.5 MB/s for
the raw zero-copy scan. The copy into the carry buffer was eating 28%. I added the
obvious fast path:

```c++
if (buf_.empty() && data.size() >= p_.max_size) { /* chunk in place, skip the copy */ }
```

Result: **1352.6 → 1352.6 ... 1377 MB/s.** A 2.7% change, i.e. noise. The 28% was still
there.

**Hypotheses.**
- (H1) The copy was never the real cost; I mis-attributed the gap. — Plausible, and it
  is the hypothesis that would have sent me off to profile the gear loop instead. Worth
  taking seriously precisely because it would have wasted the most time.
- (H2) The fast path is correct but rarely taken. — Checkable in seconds by reasoning
  about the state, no profiler needed.

**How I isolated it.** No instrumentation — just reading the state machine. `buf_` holds
whatever is left after the last cut. A chunk boundary essentially never lands exactly on
a read boundary, so after **any** feed, `buf_` holds a partial chunk and is non-empty.
The condition `buf_.empty()` is therefore true exactly once, on the very first feed. The
fast path was running for 1 MiB out of 64 MiB: 1/64th of the data, which is almost
precisely the 2.7% observed.

**Root cause.** I optimized for the state that occurs once instead of the state that
occurs always. The invariant of a streaming chunker is that the buffer is *never* empty
between feeds — that is the whole reason the buffer exists.

**Fix.** Copy only the **splice region**: the carry plus MaxSize bytes of new data, which
is provably enough to resolve the chunk straddling the read boundary. Then chunk the
remainder of the caller's buffer in place and copy only the leftover tail. Per 1 MiB read
that is ~128 KiB copied instead of 1 MiB.

The "provably enough" is the load-bearing part, so here it is: while the scan position is
at or before the end of the carry, the remaining spliced span is at least MaxSize bytes,
so every cut found there is already final (a boundary is forced at MaxSize regardless of
what follows). The loop therefore cannot stop before it has consumed past the carry.

| | before | after | |
|---|---:|---:|---|
| `Chunker::feed` (1 MiB reads) | 1352.6 MB/s | **1805.9 MB/s** | **+33.5%** |
| chunk + sha256 (full ingest CPU) | 808.6 MB/s | **966.1 MB/s** | +19.5% |

Now within 4% of the zero-copy ceiling, with bit-identical chunk boundaries (7221 chunks
before and after — the equivalence test compares both lengths *and* bytes).

**Generalizes to.** An optimization is a hypothesis, and it needs the same before/after
measurement as any other claim — mine was plausible, compiled, passed every test, and did
nothing. Being *wrong loudly and cheaply* here cost one benchmark run; believing it
would have left 28% on the floor and a comment in the code claiming otherwise. Also: when
a fast path underdelivers, check how often it is **taken** before you doubt the
measurement that motivated it.

---

## B7 💾 32 bytes of every recipe were outside the checksum — found by flipping all of them

**Symptom.** A test that flips every single byte of an encoded recipe and requires the
decoder to reject all of them:

```
CHECK_EQ(caught, encoded.size())  got: 220  vs  252
```

220 of 252 flips detected. **32 bytes were silently accepted.**

**How I isolated it.** 32 is a suspiciously meaningful number in this format — it is
exactly a SHA-256 digest. A ten-line probe printed the undetected range:

```
encoded size=252  header=64  name=8
UNDETECTED flips: 32, byte range [24..55]
```

Bytes 24..55 is precisely the `file_sha256` field of the recipe header.

**Root cause.** I computed the body CRC over the body only:

```c++
put_u32le(out.data() + 60, crc32c(ByteSpan(body, body_size)));
```

The other header fields happen to be self-checking — a corrupted `magic` or `version`
fails its own validation, and a corrupted `logical_size` or `chunk_count` fails the
"chunk lengths must sum to logical_size" consistency check. `file_sha256` is the one
field with **no independent way to tell whether it is right**, and it was the one field
the checksum did not cover. Every byte of that format was protected except the byte
range that most needed protection.

**Why it matters more than it first looks.** `file_sha256` is the end-to-end restore
check (S1). A flipped bit there does not corrupt the user's data — it corrupts the engine's
*opinion* of the user's data, so a perfectly good restore reports a digest mismatch. That
is arguably worse than a straightforward data error: it is a false alarm that an operator
cannot distinguish from a real one, and the natural response to "restore says corrupt" is
to reach for another copy.

**Fix.** The CRC now covers the header as well as the body — everything except the four
bytes of the CRC field itself, using the chaining property proven back in T1
(`crc32c(b, crc32c(a)) == crc32c(a ++ b)`), so no buffer has to be copied to skip a hole:

```c++
uint32_t c = crc32c(ByteSpan(out.data(), 60));   // header, up to the CRC field
c = crc32c(ByteSpan(body, body_size), c);        // body, skipping the field itself
```

All 252 flips are now caught. The random-mutation fuzz test's acceptance rate dropped from
24/3000 to **1/3000** — and the one survivor is a mutation that happened to write a byte's
existing value, i.e. not a mutation at all.

**Generalizes to.** "Checksum the payload" is the intuitive rule and it is the wrong one;
the rule is **checksum everything you cannot otherwise verify**. And the test that found
this was not clever — it flipped every byte and demanded every flip be caught. Exhaustive
beats representative when the input space is small enough to exhaust, and 252 bytes is
very much small enough.

---

## B8 🐛 Three of my own tests were wrong (and one was wrong in an interesting way)

Alongside B7, T3's first run produced four failures that were **defects in the tests, not
the code**. Recorded because a journal that only lists code bugs quietly implies the tests
are always right, and mine were not.

**(a) Asserting a counter after zeroing it.**
```
CHECK_GT(faults::count(faults::Op::kWrite), 0)  got: 0 vs 0
```
I called `faults::reset()` — which zeroes the counters — and *then* asserted the write
count was non-zero. The check was meant to prove the fault injection actually fired, so it
now reads the counter before disarming, and asserts `>= 2` (one short write plus its
retry) rather than `> 0`, which is the thing actually worth proving.

**(b) A name used as both a directory and a file.**
`write_file_atomic` failed inside a test that had earlier created a directory called `a`
and then tried to write a *file* called `a`. `rename()` onto an existing directory fails
with `EISDIR`. The engine behaved perfectly; my fixture was self-contradictory.

**(c) `!fits(1)` — asserting the wrong notion of "full".**
The container-capacity test filled a container with 131071-byte chunks until it refused,
then asserted no further chunk of any size would fit. But "full" is relative to what is
being offered: ~83 KB of slack remained, so a max-size chunk no longer fit while a 1-byte
chunk still would. Now it asserts `!fits(kMaxChunkLength) && fits(1)`, which is both true
and a more precise statement of the behaviour.

**(d) The interesting one: conflating "a record was lost" with "damage is detectable".**

The torn-tail test truncates a container at *every* byte offset inside its last record and
checks recovery at each one. It failed at exactly one offset — the one where the cut lands
**precisely on the record boundary**:

```
CHECK_EQ(rec.value().torn, expect_torn)  got: false  vs  true  [truncated to 2832 of 3392]
```

My model was `torn = (cut != full)`. The code's answer was better than my model. Cutting
exactly at a boundary leaves a container of five complete, CRC-valid records — **byte for
byte indistinguishable from one that was only ever five records long**. There is nothing
to detect, because nothing is damaged; a record was simply never written.

That is not a gap in the format, it is the honest limit of what any append-only log can
tell you, and it is worth being precise about: a container's own bytes can prove *"nothing
here is damaged"*, and can never prove *"nothing is missing from the end"*. Knowing whether
a chunk should exist is the recipe's job, not the container's — which is exactly the
division of responsibility SPEC §3.0 sets up, arrived at here from the opposite direction.

The test now tracks the two properties separately:
```c++
const bool complete    = (cut == full);
const bool expect_torn = !complete && (cut != last_record_start);
```

**Generalizes to.** When a test fails at exactly one point in an otherwise-passing sweep,
the single point is usually a boundary your model smoothed over — check whether the code is
right before "fixing" it. Sweeping every offset instead of three hand-picked ones is what
surfaced it; a test with three cases would have missed this and left me believing a
falsehood about my own format.

---

## B9 🔬 The memory benchmark reported a 1M-entry hash map costing 11 MiB

**Symptom.** The first run of `bench_index`, comparing our index against two
`std::unordered_map` baselines:

```
  dedupe::Index (sharded, flat)            68.6 MiB    72.0 B/entry
  unordered_map<string,Location>          133.0 MiB   139.4 B/entry
  unordered_map<Digest32,Location>         11.0 MiB    11.6 B/entry   <-- ???
```

11.6 bytes per entry for a node-based hash map whose *key alone* is 32 bytes. And the
scaling table underneath reported 0.0 MiB at every size.

**Hypotheses.**
- (H1) `unordered_map<Digest32, ...>` is secretly brilliant. — Rejected on arithmetic, not
  taste: a node holds a 32-byte key, a 12-byte value and a next pointer. 11.6 bytes is
  below the key. No implementation makes that possible.
- (H2) The RSS reader is broken. — Checked: the *first* case reported 68.6 MiB, which is
  the right order of magnitude, so the reader works.
- (H3) The measurement METHOD is broken for every case after the first. — Correct.

**How I isolated it.** The clue is which cases were wrong: the first was plausible, all
later ones were near zero. That pattern rules out the reader and points at the baseline
each case measures against.

**Root cause.** I measured RSS before and after building each structure, all in one
process. When the previous structure is freed, glibc does **not** return those pages to
the kernel — it keeps them on its own free lists. So case 2 allocated into memory that was
already resident, RSS did not move, and the delta was ~0. **An RSS delta is only
meaningful for the first large allocation in a process.**

**Fix.** Run every case in its own `fork()`ed child. Each child inherits a small baseline,
builds exactly one structure, prints its row and `_exit`s.

That immediately exposed a second, smaller bug: the header line was reprinted for every
case, because `fork()` duplicates the parent's *buffered* stdout and each child then
flushes its own copy. One `fflush(stdout)` before the fork.

**What the honest numbers turned out to be.** `unordered_map<Digest32,Location>` costs
**92.7 B/entry**, not 11.6 — and our index at that moment cost 72.9. So the real finding
underneath the measurement bug was that **our "memory-efficient" index was barely more
efficient than the baseline it was supposed to beat**, which is what led directly to B10.

**Generalizes to.** A measurement that flatters you is worth more scrutiny than one that
does not. The 11 MiB was arithmetically impossible, and noticing *that* — rather than
accepting a favourable-looking table — was the whole diagnosis. Also: process-level
metrics like RSS are not composable within a process; isolate them or use a different
instrument.

---

## B10 🔬 The index was 72.9 B/entry against a theoretical 35.6 — doubling was the culprit

**Symptom.** With the measurement fixed, the actual numbers were bad:

```
dedupe::Index      72.9 B/entry   (load 0.477, max probe 33)
unordered_map      92.7 B/entry
```

A 32-byte entry at the design's 0.75 load factor should cost 42.7 B/entry. We were at
72.9 — and only **1.27× better** than the baseline the résumé bullet claims to beat. The
spec's memory-efficiency claim was, at that moment, not true.

**Hypotheses.**
- (H1) The entry grew past 32 bytes through padding. — Rejected instantly: `static_assert`
  in `index.h` and a T0 test both pin it at 32.
- (H2) Shards are unevenly loaded, so some are oversized. — Rejected by measurement: 100k
  keys over 256 shards gave min 330 / max 455 against an expected 390. Healthy.
- (H3) The **achieved** load factor is far below the maximum. — Confirmed by the report
  itself: load 0.477, not 0.75.

**Root cause — two compounding mistakes.**

*First:* a table that doubles spends its life oscillating between `max/2` and `max` load.
The 0.75 ceiling is the value at the instant before a resize; the average is ~0.56, and
immediately after a resize it is 0.375. I had quoted the ceiling in the spec as though it
were the operating point.

*Second, and the part I had assumed away:* I expected 256 independent shards to smooth
this out — some full, some just resized. They do not. Uniform hashing means every shard
holds within a few percent of the same count, so **all 256 cross their threshold at almost
the same moment**. The aggregate load factor sawtooths just as hard as a single table
would. Sharding buys concurrency and bounded rehash *stalls*; it buys nothing at all for
average occupancy, and I had quietly credited it with both.

**Fix — two changes, each measured.**

1. **Robin Hood probing.** Entries are kept ordered by displacement, so a lookup stops as
   soon as it meets an entry closer to its home than the probe has travelled. Unsuccessful
   lookups — the common case, since every new chunk misses — drop from ~`1/(1-α)²` probes
   to ~`1/(1-α)`, and the displacement *variance* collapses. That makes a 0.90 load factor
   safe where 0.75 previously was not.
2. **Growth factor 1.5 instead of 2.**  The load band narrows from `[0.45, 0.90]` to
   `[0.60, 0.90]`, taking the average from ~0.68 to ~0.74. The cost is that capacity is no
   longer a power of two, so the home slot is computed with Lemire's fastrange (a 64×64
   multiply keeping the high half) instead of a mask — which also consumes the *high* bits
   of the key, strictly better distributed than the low bits a mask takes.

| | before | after |
|---|---:|---:|
| achieved load factor | 0.477 | **0.706** |
| bytes per entry (RSS, 1M entries) | 72.9 | **51.5** |
| max probe distance | 33 (80 at 200k) | **19** |
| vs tuned `unordered_map` | 1.27× | **1.80×** |

**Generalizes to.** A ceiling is not an operating point — for anything that grows
geometrically, the average sits well below the maximum, and quoting the maximum is a way
of being wrong on purpose. And the more useful lesson: I had assumed sharding would smooth
the aggregate load, never checked it, and built a memory claim on top. The assumption was
load-bearing and invisible until something forced a measurement. The measurement is now a
test (`CHECK_GE(load_factor, 0.55)`), so a future change that reintroduces doubling fails
in CI instead of quietly costing 40% more RAM.
