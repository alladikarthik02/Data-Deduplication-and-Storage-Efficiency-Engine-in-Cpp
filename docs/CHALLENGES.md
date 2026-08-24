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
