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
