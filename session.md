# Session Log — Hydra-Stone

> Protocol: see `.claude/skills/session-workflow/SKILL.md`.
> This file is updated at session start, after every completed step,
> and at session end. Read it BEFORE touching code.

## Done (previous sessions)

- ✅ **2026-10-01 — Engine from spec (PR #1, merged):** Implemented the
  Hydra-Stone C99 engine from the original technical spec: mmap-based
  zero-heap weight streaming, 2-bit ternary weights, coexistence-axiom
  gate, hardening, 16 unit tests, docs, CI. Reviewed afterwards;
  review findings tracked.
- ✅ **2026-10-01 — Review fixes + Web console (PR #2, merged):** Fixed
  OOB read (`layers*dim > weights_len` validation), clean state
  clamping, logs to stderr; added zero-dependency Node web console
  (`make ui`), `--json` CLI mode.
- ✅ **2026-10-01 — Real tests (PR #3, merged):** Replaced tautological
  tests with hand-computed decoder cases + seeded LCG roundtrip/variance
  tests (32 tests total, platform-fixed LCG).
- ✅ **2026-10-01 — NEON integration (PR #4, merged):** Kernel now
  genuinely wired into `step()` (was dead code); scalar fallback fixed
  on x86; honest SIMD docs. A dispatch bug was caught by the variance
  test before merge.
- ✅ **2026-10-01 — English docs (PR #5, merged):** All `.md` rewritten
  professionally in English; honest density limitation documented.
- ✅ **2026-10-01 — Android NDK port (PR #6, merged):** APK builds for
  arm64-v7a/arm64-v8a/x86_64 via CMake+JNI; verified end-to-end on an
  API-24 x86_64 emulator (no KVM). 7 errors hit and fixed — full log in
  `errors.md`, guide in `docs/ANDROID_SKILL.md`.
- ✅ **2026-10-01 — CI lint stage (PR #7, merged):** Dual-compiler
  syntax gates (gcc+clang), cppcheck, Python format gate,
  error-artifact upload. cppcheck caught a real `%u`/signed issue on
  first run.

- ✅ **2026-10-02 — Bugfix audit (PR #11, merged by user):** NEON sign
  inversion (PoC: x86 `211,388,...` vs. ARM `211,28,...`) and layer
  overflow (UB) fixed, plus 12 further findings; `test_neon_matches_scalar`
  compares NEON and scalar **in the same build** and immediately found a
  second, latent lane-offset bug. 49 tests x86 / 51 ARM. **[historical,
  superseded]** — those counts were true on that date; the suite has grown
  since. The current counts are parsed from the test binaries by
  `tools/test_count_check.sh` (see `tools/test_count_baseline.txt`), never
  written down here.
- ✅ **2026-10-02 — Web console, skills, Android APK:** see
  "Done (earlier today)" and `docs/ANDROID_SKILL.md`.

## Done (earlier today)

- ✅ Skills created + protocol files (PR #8, merged by user)
- ✅ Frontmatter v1 added (PR branch), then hardening (ASCII + quoted,
  **PR #9, merged by user at 15:46:21Z**) — main now serves valid
  skills; user invoked `/android-ndk-build` successfully → skills work
- ✅ Skill optimization after a verified end-to-end run (PR #10, merged
  by user)
- ✅ All PRs from that line are now closed: #7–#14 merged

### CI fix round (2026-10-03)

- [x] **Blocking CI failure fixed at the root.** `AssertionError: a longer
      prompt must change the output` was **not** a prompt bug: the starter
      model had `B[0] == 0`, so the decoder's `acc[0] = A[0]*token +
      B[0]*state[0]` dropped the state term entirely and prefill could not
      possibly change the output. `tools/make_model.py` now guarantees
      `A[0] != 0` and `B[0] != 0` by construction and asserts it. Verified:
      seed 5 → `[11, 33, 109]` vs `--prompt 1,5` → `[13, 41, 137]`.
- [x] **The starter model is no longer a degenerate cycle.** It emitted
      `471, 42, 471, 42` forever; it now emits
      `471, 386, 385, 386, …` - the recurrence actually contributes.
- [x] **`bench_run` initialised its out-parameter on every path** (real
      uninitialised read, only masked by statement order in the caller).
- [x] **`sscanf` → `strtoul`** for `/proc/self/statm`; a malformed line used to
      report a plausible `0 KiB` instead of "unknown".
- [x] **`bugprone-easily-swappable-parameters`**: the three mutually-convertible
      scalars (`start_token`, `steps`, `mode`) are now a `BenchRequest` struct
      passed by pointer. An enum was tried first and did **not** work — C enums
      are still implicitly convertible to every integer type, so the check
      still fired. Verified with CI's *exact* `--checks` list and file set,
      which is what the first (falsely green) local run had omitted.
- [x] clang-tidy + clang Static Analyzer: **0 findings**.
- [x] ASan+UBSan clean on the suite (71) and on the CLI bench path.
- [x] Cross-platform determinism re-verified on the **new** model across three
      targets: x86-64 host, ARM64/NEON under qemu, and the x86_64 emulator all
      emit `[471, 386, 385, 386, 385, 386, 385, 386]`.

## Current task

**Adversarial doc-audit (2 external prompts) + v2 planning for a Snapdragon 8
Elite / Android 15 tablet.** Two prompts overlap: they assert 12 doc defects,
some code gaps (Q8_0, import 429 guard), and a 6-phase v2 sparse-expert plan.

**Rule R26 + the prompt's own instruction: verify before applying.** HEAD is
`0fa13ed`, NOT the `eba5cce` the prompts cite, so every claim is re-checked
against the working tree before a single edit.

### Plan

- [x] **A0. Verify each of the 12 asserted doc defects** against the real files.
      Apply only what is true; record the ones that are stale/false.
      **All 12 confirmed real** (details below). All applied except #11,
      which is applied at the end of this section (it edits this file).
- [x] **A0b. CI assertion-count gate** — parse the count from the test binary
      and fail on drift. No hard-coded number (R32 + audit #12).
      `tools/test_count_check.sh` + `tools/test_count_baseline.txt`, wired
      into `ci.yml` for both the x86-64 and the ARM64/NEON job.
- [x] **A0c. Banned-claim lint** — `tools/docs_claim_check.sh`, four checks,
      wired into `ci.yml` (lint job) and `lint.yml` (new `docs-lint` job).
- [x] **A1. Q8_0 dequant** in `tools/gguf_reader.py` (34 B/block, w = d*q) +
      exact binary fixtures + F16-equivalence test. Seven new checks.
- [x] **A1b. `/api/models/import` single-active guard** -> HTTP 429, reset on
      every path (`try/finally`). Three new server tests.
- [x] **A1c. Tablet baseline** — **BLOCKED, NOT MEASURED.** No Snapdragon 8
      Elite / Android 15 device is reachable from this container. No number in
      this repository is presented as a device measurement. Details in the
      "A1 — code fixes" section below.
- [x] **B. `docs/FORMAT-V2.md` spec only** — no loader code in this phase.
- [x] Report: deliberation log, measured results with provenance, gates
      passed/blocked, limitations.

### A0 follow-up — a CI defect in the gate I added

While checking the deliverables rather than the code, one real defect surfaced
in the ARM count step I had wired into `ci.yml`: it ran
`test_count_check.sh ./hydra-test-arm arm`, and the script executes the binary
it is given. That binary is AArch64; the runner is x86-64. It passes here
because `qemu-user-static` has registered `binfmt_misc` in this container —
it would have depended on a GitHub runner doing the same. `test_count_check.sh`
now takes an optional command prefix and CI passes `qemu-aarch64-static`
explicitly. Logged in `errors.md`.

### A1 — code fixes, and how they were proven

**A1.1 Q8_0 dequantisation** (`tools/gguf_reader.py`). The format is short
enough to read without guessing, so it is read exactly: a 34-byte block is an
F16 scale followed by 32 signed int8 values, `w[i] = d * qs[i]`. Scales and
values are both interleaved in the file, so both are gathered with strided
slice assignment rather than a Python loop over blocks.

- `Tensor.byte_size()` derives the size for Q8_0 and **raises** when the
  element count does not tile whole blocks (rules.md R14 — a length-deriving
  field that does not divide is a malformed file, not something to round).
- `decode_rows()` reads the blocks that cover a row range and slices the
  edges, so a row length that is not a multiple of 32 still decodes exactly.
- `decodable_candidates()` / `quantised_names()` now separate "can be read"
  from "is refused". Q8_0 is block-quantised **and** decodable; saying
  otherwise would be the same class of error this task exists to fix.

*Proof:* seven new checks in `tools/gguf_test.py` (38 total, up from 31), plus
one live-server test (37 total, up from 33). **Four deliberate decoder bugs**
were injected; all three value checks caught every one:

| Injected bug | `_q8_0_exact` | `_q8_0_row_slice` | `_q8_0_matches_f16` |
|---|---|---|---|
| scale taken from the neighbouring block | CAUGHT | CAUGHT | CAUGHT |
| the 32 codes rotated by one | CAUGHT | CAUGHT | CAUGHT |
| int8 sign flipped | CAUGHT | CAUGHT | CAUGHT |
| scale ignored, raw int8 returned | CAUGHT | CAUGHT | CAUGHT |

The first mutant originally **passed** — every fixture used one repeated
scale. That was a real coverage gap, fixed by giving each block a different
power-of-two scale, which keeps the F16-equivalence check an equality rather
than a tolerance. Logged in `errors.md`.

**A1.2 Single-active-import guard** (`server.js`). At most one
`POST /api/models/import` at a time; a concurrent one gets **HTTP 429** with
`Retry-After` and `busy: true`. The flag is released in a `finally`, so every
exit path — success, 400, 413, converter crash, client hang-up — returns it.

*Proof:* three new server tests. The 429 test polls for the `.import-*.gguf`
temp file rather than sleeping, so it cannot silently pass by never
overlapping. **Negative control:** with the guard disabled the test fails with
`got 200`; the server was restored byte-identically afterwards (`diff` against
a pre-test copy).

**A1.3 Tablet baseline — BLOCKED, NOT MEASURED.**

No Snapdragon 8 Elite / Android 15 tablet is reachable from this container.
**No peak RSS, no ns/token, no bytes/token and no page-fault figure exists for
the target device**, and none is estimated anywhere in this repository. The
one deliberate exception is spelled out in `docs/FORMAT-V2.md` §6.3 as an
explicitly-marked placeholder: the planning arithmetic is parameterised on
`B_eff` and the two bracketing estimates are labelled **invalid**, because
they assume a stream that sparse expert reads are not. Colibri's own measured
figures (12.7 GB of routed experts per token; 0.05–0.1 tok/s cold on a 25 GB
box) are cited with their provenance as an order-of-magnitude check, not as
this project's result.

A desktop `pread` micro-benchmark in this container was considered and
**rejected**: it measures NVMe behind a page cache under Linux, not UFS 4.0
under Android, and a number from the wrong storage stack is precisely the
failure this whole task exists to fix. Recorded in `errors.md`.

### A0 — what the audit found, verified against the working tree

HEAD at the time of the audit was `0fa13ed`, not the `eba5cce` both prompts
cite. Every finding was re-checked by reading the actual file before editing
it; all twelve turned out to be real, not stale:

| # | Finding | Verified against | Fixed in |
|---|---|---|---|
| 1 | "Zero-RAM" in the README title | `README.md:1` vs. the `errors.md` entry that already called it unproven | `README.md` |
| 2 | "step() walks the whole weight region per token" | `docs/ARCHITECTURE.md`; contradicts `hydra_build_aggregation()` in `src/hydra_engine.c` | `docs/ARCHITECTURE.md` |
| 3 | FORMAT rule 7 describes a per-token traversal | `docs/FORMAT.md`; `hydra_engine_step()` never reads the weight region | `docs/FORMAT.md` |
| 4 | README size table implies v1 holds 1.5 B parameters | `include/hydra_model.h` caps weights at 256 KiB | `README.md` |
| 5 | "packs 4 weights per byte, which doubles file size" | internal contradiction with line 359 of the same file | `README.md` |
| 6 | gpu_bench example JSON says `"measured":true` under "NOT MEASURED" | `tools/gpu_bench/README.md` | same |
| 7 | Five conflicting counts in the skill file | `.claude/skills/engine-ci-verify/SKILL.md` | same |
| 8 | `rules.md` R21 hard-codes 49 | `rules.md` | same |
| 9 | "the NEON path is active automatically" | `android/README.md`; no physical ARM device has ever run this | same |
| 10 | Performance numbers with no host named | `README.md` Performance section | same |
| 11 | Historical sections with drifted numbers | `session.md` | `session.md` |
| 12 | CI enforces no test count | `.github/workflows/ci.yml` | `ci.yml` + new scripts |

Findings 7 and 8 turned out to be worse than the audit said: after the first
pass removed the counts the new lint found **three more** stale ones in the
same skill file (lines 66, 69, 130). That is the falsifiability evidence for
the lint, and it is why the lint had to run before the gate was called green.

### Measured in this task (provenance)

| Fact | How it was measured | When |
|---|---|---|
| x86-64 assertion count = 71 | `./hydra-test`, parsed from its own output | 2026-10-03, this container |
| ARM64/NEON assertion count = 77 | `aarch64-linux-gnu-gcc-12 -static` under `qemu-aarch64-static` | 2026-10-03, this container |
| GGUF suite = 38 checks, 0 failed | `python3 tools/gguf_test.py --engine ./hydra-run` | 2026-10-03 |
| Server suite = 36 tests, 0 failed | `node --test tools/server_test.js` | 2026-10-03 |
| Q8_0 dequant is exact | 4 hand-built blocks, 4 different scales, compared against `d*q` computed with `struct` | 2026-10-03 |
| Import guard returns 429 | live server, two overlapping POSTs, second one while the first's temp file exists | 2026-10-03 |

### Three-role adversarial review (before implementation)

#### Role 1 — Critic (physical and semantic constraints)

The strongest objection is to the *premise* of the v2 phases, not to any
detail of them. Every storage figure quoted in the prompts is a **sequential**
figure, and MoE expert streaming is not sequential.

- **Evidence (primary source, fetched 2026-10-03):**
  benchmarks.ul.com's Xiaomi Pad 8 Pro page — the same Snapdragon 8 Elite
  class of device — reports, as the median of user submissions to PCMark for
  Android Storage 2.0: internal **sequential read 3,328 MB/s** but internal
  **random read 55 MB/s**. Same page, same device, same run.
- **Evidence (primary source, fetched 2026-10-03):** Colibri's own README
  states GLM-5.2's routed experts cost **12.7 GB per token**, and that the
  project measures **0.05–0.1 tok/s cold** on a 25 GB desktop, which it calls
  "the proven floor".
- **Therefore:** `tokens/s <= B_eff / bytes_per_token` evaluated at
  3,328 MB/s gives an upper bound of ~0.26 tok/s for GLM-5.2 — and that
  bound *assumes a perfectly sequential stream, which the workload is not*.
  Evaluated at the same device's measured random-read figure the number is
  two orders of magnitude worse. The planning model is sound; using
  `B_seq` where the model says `B_eff` is the error.
- **Second objection:** dense streaming and sparse expert streaming are
  different problems wearing the same word. Dense streaming can prefetch a
  contiguous run and amortise the seek. Sparse routing is, per layer, a few
  `pread` calls at addresses the previous layer did not predict perfectly —
  Colibri's own **71.6%** is one-layer-ahead *predictability*, which is an
  upper bound on what prefetch can save and is **not** a cache-hit rate.
  Treating 71.6% as a hit rate would overstate Phase E by roughly the
  difference between those two quantities.
- **Third objection:** the prompts' Phase D budget line
  ("resident + slots*expert_bytes + state/KV + work buffers + system
  headroom") omits the file-backed page cache, which is physical RAM on
  Android and is charged to the app's RSS until it is reclaimed. A budget
  that does not name it is not a budget.

#### Role 2 — Math & Web Researcher (labels: MEASURED / SPEC / ESTIMATE)

| Quantity | Value | Label | Source |
|---|---|---|---|
| PCMark internal sequential read, Xiaomi Pad 8 Pro (SD 8 Elite) | 3,328 MB/s | **MEASURED** (median of user submissions, *not* a lab result; device runs Android 16, not 15) | benchmarks.ul.com, fetched 2026-10-03 |
| PCMark internal random read, same device | 55 MB/s | **MEASURED** (same caveats; block size not stated on the page) | same |
| Samsung UFS 4.0 sequential read | 4,200 MB/s | **SPEC** — manufacturer peak datasheet, not any device's sustained rate | semiconductor.samsung.com |
| Colibri resident dense weights (GLM-5.2) | ~9.9 GB | **MEASURED** by that project, reported in its README | github.com/JustVugg/colibri README, fetched 2026-10-03 |
| Colibri routed experts | 19,456 (75 layers x 256 + MTP head), ~19 MB each int4 | **MEASURED** by that project | same |
| Colibri system RAM requirement | 16 GB min / 24 GB comfortable | **SPEC** of their container — *not* the dense size | same |
| Colibri 71.6% | one-layer-ahead routing predictability | **MEASURED** by that project; **not** a hit rate | same |
| Colibri routed bytes per token | 12.7 GB (GLM-5.2), 4.5 GB (DeepSeek V4.1 Flash) | **MEASURED** by that project | same |
| `B_eff` on the target tablet | — | **NOT MEASURED. Blocked.** | no device reachable |
| Any tokens/s figure for this project on the target tablet | — | **NOT MEASURED. Blocked.** | no device reachable |

Reconciliation against the prompts' own numbers: everything they list for
Colibri checks out against the README, **with the qualifications intact** —
the 16/24 GB figures are system RAM, the 71.6% is routing predictability, and
4,200 MB/s is a datasheet peak. Nothing in the prompts was found to be false.

#### Role 3 — Systems Architect (staged design, gates, responses)

Responding to the Critic's strongest objection directly: **do not build Phase
D before the disk can be characterised.** The Critic is right that a bounded
LRU cache is the right design and also that its usefulness is bounded by a
number nobody has measured. The resolution is to make `B_eff` a *required
input to the gate*, not a design constant:

- Gate D becomes "measured RSS within budget **and** measured `B_eff` on the
  same device in the same session". A cache that is within budget but slower
  than streaming straight through is a failed gate, not a passing one.
- `docs/FORMAT-V2.md` therefore carries a worked budget whose throughput
  section is explicitly parameterised on `B_eff`, with the worked number
  marked **PLACEHOLDER — NOT MEASURED**, and the Colibri figure used only as
  an order-of-magnitude sanity check with its provenance attached.
- Both bounds on every offset/length/count are specified as a rule, because
  this repository's own history has a real missing-lower-bound bug; the spec
  states it as a checklist item rather than as advice.

#### Deliberation summary

| Disagreement | Resolution |
|---|---|
| Critic: "3,328 MB/s makes the MoE plan look fine." Architect: "it does not, and the prompts' own model already says so." | **Unanimous against the sequential figure.** `B_eff` stays a measured input; the spec and every planning number are parameterised on it. The 3,328 MB/s figure is retained only as a labelled *sequential* upper bound, never as `B_eff`. |
| Critic: "71.6% sounds like a cache hit rate." Researcher: "it is not; the README says 'predictable one layer ahead'." | **Unanimous.** Phase E's gate measures predicted-vs-actual routing and reports hit rate separately. The 71.6% figure appears in the spec only with that wording. |
| Architect wanted the tablet benchmark run first. Critic: it is the one thing that cannot be faked, and also the one thing unavailable. | **Split.** A1c is recorded as **BLOCKED, NOT MEASURED** with the reason, and the harness requirement is written down so the run is a single command later. No estimate is promoted to a measurement. |
| Researcher proposed adding a synthetic micro-benchmark for `pread` on the container as a stand-in. Architect: a desktop container's `pread` on NVMe says nothing about UFS 4.0 under Android. | **Rejected.** A number from the wrong storage stack would be exactly the failure this whole task exists to fix. Recorded in `errors.md`. |

### Phase B — `docs/FORMAT-V2.md` (specification only)

**No v2 loader was written.** `src/hydra_v2_reader.c` does not exist and no
v2 file can be opened; the document says so in its first line. What it does
specify: 64-bit little-endian offsets and lengths; a 128-byte header with
`file_size` so a truncated transfer is detectable; a resident trunk (router,
embeddings, norms, shared experts) that is the only thing mapped at open; an
expert directory of 48-byte entries carrying `(layer, expert_id, offset,
length, first_element, type_id, crc32)`; contiguous payloads assigned in
directory order so one expert is one `pread`; routing metadata with
`v2.top_k` **required, never defaulted**; both bounds validated on every
offset, length and count with the missing-lower-bound bug named as the reason
the rule exists; the supported architecture and tensor-type tables written
out explicitly, including the ones deliberately **not** implemented; and a
worked 10 GB memory and throughput budget whose `B_eff` is a clearly-marked
unmeasured placeholder.

Gate B is **spec review**, and this is the document under review. Four open
questions are listed at the end rather than silently decided — the largest is
whether CRC-32 is worth a pass over every byte read when read bandwidth is
already the bottleneck.

### Gates

| Gate | Status | Evidence |
|---|---|---|
| A0 — doc & CI integrity | **PASS** | 12/12 findings fixed; both new gates wired into `ci.yml` and `lint.yml`; `git status` shows the workflow edits |
| A0 negative controls | **PASS** | `test_count_check.sh` with a wrong baseline → exit 1; `docs_claim_check.sh` on planted claims → exit 1 (all four checks proven) |
| A1 — code fixes, x86-64 | **PASS** | C 71/71, GGUF 38/38, Node 37/37, ESLint clean, cppcheck clean |
| A1 — code fixes, ARM64/NEON | **PASS (cross-build under qemu)** | 77/77, count gate green |
| A1.3 — tablet baseline | **BLOCKED — NOT MEASURED** | no target device reachable |
| B — v2 specification | **DELIVERED, AWAITING REVIEW** | `docs/FORMAT-V2.md` |
| C–F — reader, cache, prefetch, benchmark | **NOT STARTED, BY DESIGN** | B must be reviewed first; D and F need the tablet |

### Limitations that stand

- No physical ARM Android device has run this. ARM evidence is an AArch64
  cross-build under qemu plus a native Apple Silicon CI runner.
- The Q8_0 support is in the **Python GGUF import path only**. There is no C
  reader for it yet, so a `.hydra2` file containing Q8_0 is specified, not
  implemented.
- `B_eff`, peak RSS, ns/token, bytes/token and page faults on the target
  device: **not measured**, not estimated.
- The Q8_0 dequantiser is correct for `w = d * q` on 32-value blocks. It is
  **not** a general GGUF block-format implementation: `Q8_1` (which carries a
  second `min` term), `Q4_0`, `Q4_1`, `Q5_*`, the K-quants and `MXFP4` are
  still refused by name.
- The release APK is debug-key signed; a real key needs the four
  `HYDRA_KEYSTORE*` repository secrets.

### Files changed in this task

**Modified:** `README.md`, `docs/ARCHITECTURE.md`, `docs/FORMAT.md`,
`docs/GGUF-IMPORT.md`, `android/README.md`, `tools/gpu_bench/README.md`,
`.claude/skills/engine-ci-verify/SKILL.md`, `rules.md`, `errors.md`,
`session.md`, `.github/workflows/ci.yml`, `.github/workflows/lint.yml`,
`server.js`, `public/app.js`, `tools/gguf_reader.py`, `tools/gguf_to_hydra.py`,
`tools/gguf_inspect.py`, `tools/gguf_test.py`, `tools/gguf_fixture.js`,
`tools/server_test.js`.

**New:** `docs/FORMAT-V2.md`, `tools/test_count_check.sh`,
`tools/test_count_baseline.txt`, `tools/docs_claim_check.sh`.

### Constraints honoured

- **R32 / hard warning 2:** the target tablet is NOT available here. Any
  device number is reported "blocked, not measured" — never estimated.
- **R14/R17:** no merge; v1 stays byte-compatible; v2 is separately versioned.
- Hard warning 4: supported architectures/tensor types listed explicitly.

## Previous task (superseded)

**GGUF import + measured load performance + a truthful README, all verified on
a running emulator.** 20 GB support (header v2) is still open.

### Result

- [x] **GGUF -> .hydra converter** (`tools/gguf_reader.py`,
      `gguf_to_hydra.py`, `gguf_inspect.py`), Python stdlib only — no numpy, no
      torch. Absmean ternarisation (BitNet b1.58), half-away-from-zero
      rounding, streamed so a multi-GB file need not fit in RAM.
      `tools/gguf_test.py` (31 checks) converts a synthetic GGUF and loads
      the result with the **real C engine** — a self-consistent converter
      would not survive that.
- [x] **`POST /api/models/import`** streams the file, converts, validates with
      the same header analysis as upload, deletes the source GGUF, and returns
      the conversion report (tensors read, rows sampled, `gamma`, `mean|w|`,
      density, rescale, peak, vocabulary). Converted models are flagged in
      `/api/models` and appear in the chat selector.
- [x] **Android runs by itself.** Confirmed on an API-24 x86_64 emulator:
      the app loads `starter.hydra` and completes inference without input.
- [x] **Layout bug found on the device, not by reading code.** On 320x640 the
      root `LinearLayout` pushed the token log below the visible area — the
      engine ran and showed nothing, which is exactly what "it does not run"
      looks like. One `ScrollView` around the whole screen; verified by
      decoding `screencap` (12 content bands, lowest y=623 of 640).
- [x] **Load measurement, not an assertion.** `--bench` now reports
      `load_ms`, `rss_before_kb`, `rss_after_kb`, `rss_delta_kb`. An explicit
      `MADV_DONTNEED` was tried and **reverted**: `MADV_SEQUENTIAL` already
      frees the pages behind its scan and the explicit call made RSS *worse*.
- [x] **Demo/placeholder content removed.** `assets/demo.hydra` ->
      `starter.hydra`; `tools/make_dummy_model.py` -> `tools/make_model.py`;
      redundant string removed (Android Lint back to **0 issues**).
- [x] **README audited claim by claim and corrected.** It was wrong in eight
      places — see `docs/GGUF-IMPORT.md` and the corrections below.
- [x] **The shipped starter model was not in git.** `.gitignore` ignores
      `*.hydra`; `demo.hydra` had been force-added long ago, so the rename to
      `starter.hydra` silently deleted the asset from the repository. Local
      builds still had it, fresh checkouts did not — CI failed with
      “starter.hydra differs from tools/make_model.py output”, and the APK
      content assertions in `ci.yml`/`release.yml` would have failed too.
      Fixed with a `!android/app/src/main/assets/starter.hydra` negation rule
      plus staging the file. The autostart gate now asks the question that
      actually matters (`git check-ignore --no-index`), because the
      byte-comparison it sat next to runs locally and can never catch this.

### README claims that were false and are now fixed

| Claim | Was | Now |
|---|---|---|
| generator | `make_dummy_model.py` (deleted) | `make_model.py` |
| test count | 32 / 49 / 51 / 56 / 62 | **71 x86, 77 ARM** (measured) |
| cancel | "engine process deliberately left running" | real `SIGTERM`/`SIGKILL`, 409 `cancelled` |
| model asset | "packed demo model" | `assets/starter.hydra` |
| inspect example | `models/demo.hydra` | `models/starter.hydra` |
| Android button | "Import .hydra model…" | "Import model…" |
| import report | "peak memory, conversion time" | the fields the report actually has |
| roadmap | GGUF import unchecked | done; safetensors still open |

### Still open

- 20 GB models need header v2 (64-bit offsets, 4-weights-per-byte packing).
  The v1 ceiling is **256 KiB of weights** (dim ≤ 64 × layers ≤ 4096 bytes) —
  a format limit, not a RAM limit.
- No physical ARM device was available; all Android numbers come from an
  x86_64 emulator under TCG.

### Earlier: phase P0 (delivered)

- [x] **413 actually delivered.** `streamToTmpFile()` called `req.destroy()`
      before the response, so the client got an empty reply (measured:
      curl transport error, 0 bytes). Order is now stop reading -> answer ->
      close, with a `Content-Length` pre-check plus the streaming guard, and
      the body carries `limitBytes` / `limitHuman` / `raiseWith`.
      Negative control (re-adding `req.destroy()`) reproduces `ECONNRESET`.
- [x] **Upload cap configurable**: `HYDRA_MAX_UPLOAD` (`1MiB`, `2GB`,
      plain bytes), default 64 MiB. A typo falls back to the default
      instead of removing the limit.
- [x] **Magic-specific messages**: GGUF, ZIP, ELF, PNG, gzip, bzip2 are
      named with advice; unknown magics still state both values. The
      generic "wrong magic" string is gone.
- [x] **`GET /api/models/inspect?path=`**: header values + one entry per rule
      (`id`, `ok`, `message`) + `violations` + the current format ceiling.
      200 even for a broken model (a diagnosis is a successful answer).
      Paths stay inside the project: 400 outside, 404 when absent.
- [x] **One analysis, two callers**: upload refusal and inspect report quote
      the same first violation; a test asserts it.
- [x] **Android** mirrors the same rule ids, logs the header and *every*
      violated rule, names GGUF, and sums `offset + len` in `Long`.
- [x] **Console** shows the violated rules under any model marked
      *invalid header*, and appends how to raise the upload cap.

## Plan

- [x] Web verification of all CVE IDs from the audit:
      CVE-2025-2439 and CVE-2025-2445 (Cortex.cpp, confirmed via
      Tenable/Snyk), CVE-2025-53630 (`gguf_init_from_file_impl`,
      NVD), CVE-2026-27940 (same function, later fix),
      CVE-2026-33298 (`ggml_nbytes`, CVE.org/Red Hat),
      CVE-2026-70638 (llama.cpp Android JNI `new_1batch`, builds
      b1886–b7445, NVD) — **all IDs and assignments correct**
- [x] Web research on the best linters of 2026 (C / Android / JS)
- [x] Every tool installed locally and **verified with a negative
      control** (revert the fix temporarily, the gate must fail):
      - `gcc -fanalyzer`, `clang --analyze`, cppcheck → no findings
      - clang-tidy → 3 real `bugprone-easily-swappable-parameters`,
        0 after NOLINT with a justification
      - flawfinder `-m 2` → 29 false positives, `-m 4` → 0
      - **ASan+UBSan → `signed integer overflow: 2147483640 + 127`**
        on the unfixed code, 0 on the fixed code
      - Android Lint → 5 warnings, "No issues found" after the fix
      - ESLint 10 → 7 errors, 0 after the fix
- [x] **New findings from the audit reproduced and fixed:**
      - A symlink in the model path bypassed `safeModelPath()`
        (verified: the error revealed the size of `/etc/hostname`)
      - The server leaked internal engine details to the client
      - 6 global functions in the frontend (`no-implicit-globals`)
- [x] Hardening: `O_NOFOLLOW` in the loader + `realpath`/`lstat` in the
      server, generic 500 responses, IIFE in the frontend
- [x] Regression test: `test_engine_rejects_symlink()` (fails without
      `O_NOFOLLOW` — negative control executed)
- [x] Android: vector icon (adaptive + fallback + monochrome), all UI
      strings as resources, `plurals`, backup rules, no orientation
      lock → **Android Lint: no findings**
- [x] Release: signed `assembleRelease` configuration with keystore
      secret **and** debug fallback; version derived from the Git tag
- [x] `.github/workflows/lint.yml`: 8 jobs (c-lint, sanitizers, jni-lint,
      web-lint, release-config, android-lint, semgrep, codeql)
- [x] `.github/workflows/release.yml`: 5 jobs, parallel Linux/macOS/
      Android builds, parity gate, APK verification, SHA256SUMS
- [x] Release chain replayed locally end to end (binaries + APK +
      checksums)
- [x] **Release APK installed and executed on the emulator**:
      `adb install` → Success, token sequence
      `211,36,185,410,7,40,197,478` — identical to host and ARM/NEON
- [x] `errors.md`: new entries; `rules.md`: R25–R30 (linter
      falsifiability, web verification, error leaks, symlink paths)
- [x] README: release/APK section, linter table, project structure
- [x] Commit (no checkout) + PR — **no merge**
- [x] PR #12 and #11 merged by the user (main: `cfdb34f`)

## Round 2026-10-02 (CI fix round after the merge)

- [x] Status clarified: `main` was green except for `Android lint`
      (13 of 14 jobs green) — the cause is **not** the code
- [x] Root cause verified in the log: `android-actions/setup-android@v3`
      runs `sdkmanager tools`; that package no longer exists
      (`Warning: Failed to find package 'tools'` → exit 1)
- [x] Fix: download/unpack the cmdline-tools explicitly + PATH +
      explicit license acceptance, in **lint.yml and release.yml**
- [x] New branch `fix/android-lint-cmdline-tools` built directly on
      `origin/main` (plumbing, **no `git checkout`** — R17/R20),
      PR opened, **not merged** (R14)
- [x] **PR #13: 13 of 13 checks green.** The first run still reported
      `OldTargetApi` — caused by the runner's prebuilt SDK
      (platforms 35/36) while only 34 is installed locally. Fix: an own
      SDK root containing nothing but the pinned packages
- [x] Incidental fix: `release.yml` addressed the build-tools through
      the hardcoded `/opt/android-sdk`, which does not exist on the
      GitHub runner — now derived from `ANDROID_HOME`
- [x] **PR #13 approved and merged by the user** (head `2fd2223`)
- [x] **Verified on `main`** (push commit `60a01f6`): workflow
      `Lint & Security` 8/8 green (Android lint 2m17s,
      `No issues found.`, NDK C compilation successful), workflow
      `CI` 4/4 green — both runs `conclusion: success`

## Round 2026-10-02 (making the release workflow manually triggerable)

- [x] Finding: `workflow_dispatch` existed but was only usable with a
      mandatory tag input; without input it produced
      `version=main`, `versionCode=0` — a release named "main" would
      have been published
- [x] `tag` optional, `publish` boolean (default `false`), dry run with
      the synthetic tag `v0.0.0-ci.<run_number>`, `publish` job gated
      via an `if` condition; tag-push trigger unchanged
- [x] Tag validation `vMAJOR.MINOR.PATCH`, `versionCode >= 1`, inputs
      passed via `env:` instead of interpolation (no shell injection)
- [x] `tools/ci_release_version_test.sh`: 5 cases (tag push, publish,
      dry run, invalid tag, publish without tag) — **negative control
      executed**, the test fails (exit 1) on a broken file
- [x] New job `release-config` in `lint.yml`: YAML validation of all
      workflows + version-logic test — **green** on PR #14 (14 of 14
      checks)
- [x] PR #14 opened; end-to-end dispatch attempted but blocked by the
      environment: `gh workflow run` → **HTTP 403**, the managed GitHub
      app may not create a `workflow_dispatch` event. The manual route
      (Actions tab → Run workflow) uses the user's permissions and is
      the only remaining test step
- [x] **PR #14 merged by the user** (merge commit `b8a4d97`)
- [x] **Verified on `main`** (push commit `b8a4d97`): workflow `CI`
      4/4 green, workflow `Lint & Security` 9/9 green including the new
      `Release config` job and `Android lint` — both runs
      `conclusion: success`

## Round 2026-10-02 (documentation language)

- [x] Audit of every tracked `.md` file for German content:
      `rules.md`, `README.md`, `docs/*.md`, `android/README.md` and the
      skill files were already English (only a few occurrences of the
      German word for "failures" in `engine-ci-verify/SKILL.md`
      remained)
- [x] `session.md` and `errors.md` translated to English, preserving
      every entry, cause, fix and verification result
- [x] Test output string aligned with the documentation: the C test
      suite prints `Tests, failures` in English, and the regression
      test expectation in the skill file matches
- [x] Verification: `make test` → `=== 49 Tests, 0 failures ===` **[historical,
      superseded]** — the count has grown since; see
      `tools/test_count_baseline.txt`. Final
      grep over all tracked `.md` files finds no German left
- [x] Final grep for German leftovers in `.md` files → none
- [x] PR #16 merged by the user (merge commit `1cc31d7`), main verified
      green: CI 4/4 and Lint & Security 9/9

## Round 2026-10-02 (build everything on main, verify the JNI bridge)

- [x] **Analysis — why main never built the APK:** `release.yml` only ran
      on `v*.*.*` tags or a manual dispatch, and `ci.yml` contained no
      Android job at all. So the app was never built on a normal push,
      and no release appeared because no tag existed.
- [x] New job `android-apk` in `ci.yml`: builds `assembleDebug` +
      `assembleRelease` on every push and pull request, verifies the
      signature with `apksigner`, checks all three ABIs and the model
      asset with `aapt`, and uploads the APK as a workflow artifact
- [x] New job `jni-signatures` in `ci.yml`: `javac -h` derives the JNI
      header from `HydraBridge.kt`; `tools/jni_signature_check.sh`
      proves every native method exists in `hydra_jni.c` with a
      matching parameter list. **Negative control executed** — a bridge
      declaring one unimplemented method makes it exit 1
- [x] Reusable composite action `.github/actions/setup-android` so the
      SDK bootstrap exists once instead of per workflow
- [x] CI translated to English: comments, step names, `::error::`/
      `::notice::` messages and the release notes body
- [x] **The new APK gate immediately caught a real bug:**
      `android/app/src/main/assets/demo.hydra` was never tracked by git,
      so every clean checkout — i.e. every CI run and the first tag run —
      built an APK without the demo model. The asset is now tracked and
      gate 1e in `ci.yml` compares it byte for byte against
      `tools/make_dummy_model.py` (deterministic: three runs, one sha256)
- [x] APK verification hardened: `aapt` **and** `unzip` listings are
      printed and the archive is checked with `unzip`, artifact upload
      also runs on failure
- [x] Verified locally and on CI: `assembleDebug` + `assembleRelease`
      build, `apksigner verify` passes (debug-key fallback),
      `libhydra.so` for arm64-v8a/armeabi-v7a/x86_64 and
      `assets/demo.hydra` present
- [x] PR #18: **17/17 checks green**, including the two new jobs
      `Android APK build` and `JNI bridge (javac)`
- [x] PR #18 merged (merge commit `03421a9`) — 16/16 checks green
- [x] `v1.0.0` pushed → **first `release.yml` run ever failed** in two
      jobs, both configuration bugs only a real tag run can hit:
      `dist/` was never created in the Linux CLI job, and an unset
      GitHub secret arrives as an EMPTY string, so
      `file(System.getenv("HYDRA_KEYSTORE")!!)` called `file("")`
- [x] PR #20 fixes both (keystore path via `takeIf { isNotBlank() }`),
      16/16 green; replayed locally with `HYDRA_KEYSTORE=""`
- [x] Tag `v1.0.0` re-pointed at the fixed `main` tip `a0ba722`
- [x] **Release published:** https://github.com/JONIMONI09/BestAI/releases/tag/v1.0.0
      - `hydra-stone-1.0.0-android-arm64v8a-armeabiv7a-x86_64.apk`
        (671 664 B, signature verified, `libhydra.so` for all three
        ABIs, `assets/demo.hydra` present, versionCode 10000)
      - `hydra-stone-debug.apk`, three CLI binaries, `SHA256SUMS.txt`
- [x] Downloaded the released APK and its checksum:
      `f5dced81…0eda7` — identical, integrity chain verified end to end

## Status / Notes

- The most important insight of that round: **UBSan is the only gate
  that reports real arithmetic UB.** Every static analyzer was blind to
  it. That is why the sanitizer job is mandatory, not optional.
- `flawfinder` stays at level 4 — at level 2 it flags every
  `fopen`/`mkstemp` and every char array, which is only noise.
- CVE-2026-70638 (llama.cpp JNI multiplication overflow) was the bug
  class `hydra_jni.c` was searched for: the JNI bridge does not
  _allocate_ anything from header fields, it only clamps, so the class
  does not apply.
- Open PRs: none. #7–#14 are all merged.
- Remaining manual step: one *Run workflow* execution from the Actions
  tab on `main` (empty tag = dry run) exercises the release chain
  end to end. The automated credential cannot dispatch workflows
  (HTTP 403, no `actions: write`).
## Round 2026-10-02 (console rebuild: settings, chat, real training)

- [x] Analysis: the old console was a three-input demo page in German with
      no model handling, no settings and no training; `models/demo.hydra`
      was not even tracked, so a clean checkout had no model at all
- [x] `tools/hydra_train.js`: real trainer, mirrors `hydra_engine_step`
      line by line; hill climbing on the whole-sample score, best
      snapshot returned, writes a real `.hydra`
- [x] `tools/console_train_test.js`: trains, writes the file, then runs the
      COMPILED `hydra-run` and asserts the token stream matches bit for
      bit; includes a negative control (garbage model must be rejected)
- [x] Server: `/api/models`, `/api/train`, `/api/vocab` (GET+PUT), plus
      auto-generation of the demo model on startup
- [x] UI rewrite: modern dark console, chat with vocabulary mapping,
      model picker, live token chart, state heat strip, settings drawer,
      training panel showing real before/after accuracy and the engine
      verification
- [x] Verified locally: ESLint clean, `node --check` clean, trainer test
      green, server smoke tests over all endpoints, path traversal still
      404, APK/CI untouched
- [x] CI: `web-lint` now also runs the trainer test against the real engine
- [ ] PR + merge — **user approval needed** (R14)

## Round 2026-10-02 (automatic release on every main push)

- [x] Analysis of the screenshots: repo showed "No releases published",
      0 tags; the hand-made tag `v1.0.0` pointed at `03421a9`, a commit
      from *before* the two release-run fixes, and the release itself was
      gone. Root cause: `release.yml` was triggered only by
      `push: tags`, so a push to `main` never built anything and a tag
      alone never produced a release.
- [x] `release.yml` now also runs on `push: branches: [main]` and decides
      on its own: no tags -> `v1.0.0`; tag on an older commit -> patch
      bump; tag already on this commit -> reuse. Dry-run tags
      (`v0.0.0-ci.<run>`) never count as a base.
- [x] `ensure-tag` job creates the tag through the checked-out
      credentials. A token push does not trigger workflows, so the build
      runs exactly once (no double build from the tag trigger).
- [x] `publish` is now an explicit upsert: `gh release view` ->
      `gh release edit` + `gh release upload --clobber`, otherwise
      `gh release create --target <main sha>`. A verify step asserts that
      all five expected assets are attached, so a release can never be
      "successful" while missing the APK.
- [x] `tools/ci_release_version_test.sh` extended from 5 to 10 cases
      (main push without tags, dry-run tag, new commit -> patch bump,
      semver max over several tags, tag already on main) and it checks
      the new `create_tag`/`create_release` outputs. All 10 pass; the
      negative control (a deliberately broken copy of the workflow) still
      fails, so the test is not vacuous.
- [x] Two real bugs were caught by the new cases: `while read -r name
      sha` swallowing the rest of a multi-line tag list, and a missing
      `IFS=.` in the semver split (both produced a wrong version
      silently).
- [x] Verified: all workflow YAML parses, `bash -n` clean on every `run:`
      block of `release.yml`.
- [ ] PR + merge — **user approval needed** (R14). After the merge the
      next push to `main` creates `v1.0.0` with the real artifacts,
      because the repository currently has no tag and no release at all.

## Round 2026-10-02 (audit: UI/UX bugs, model loading, engine prompt)

Audit of the reported findings against the actual code first. Result:

- **Part 4 was already done in main.** `hydra_neon.h` uses the
  mask-then-subtract decode (`vsubq_u8(p1n1)` on 0x01-normalised masks),
  `HYDRA_MAX_LAYERS` 4096 is enforced with `rc=-11`, the accumulator is
  `int64_t`, and `test_neon_matches_scalar` exists. Nothing was changed
  there except strengthening the test.
- **Part 3.1/3.2 were already done.** `ci.yml` builds and tests on x86-64
  (ubuntu + macos), runs the AArch64/NEON build under qemu, and uses
  `cppcheck --error-exitcode=2`; sanitizers live in `lint.yml`.
  `release.yml` already builds the APK, verifies it and attaches it to the
  release, so a second `android-release.yml` would only duplicate it.

Work actually done:

- [x] Engine: `hydra_engine_prefill()` + CLI `--prompt`, JSON echoes the
      prompt. C tests: prefill equals a manual prompt run, prefill is not a
      no-op, n=0/NULL handling (7 new assertions).
- [x] NEON: new `test_neon_all_ternary_codes` writes a model whose weights
      contain **all four** 2-bit codes (00/01/10/11) and compares NEON with
      the scalar reference, plus a negative control proving that a +1 and a
      -1 model really do differ. Re-introducing the original `vsubq` sign
      bug was proven to fail the suite (3 failures, exit 1).
- [x] Console 1.1/1.2/1.3: reverse vocabulary map, re-render on view
      toggle, full prompt to the engine.
- [x] Console 1.4: Send/Train disabled while a request runs, Cancel aborts
      via `AbortController` (the engine process is deliberately left
      running), `finally` always re-enables.
- [x] Console 1.5: real `<label>` for the composer, focus trap + Escape +
      focus return in the drawer, state cells 9.5px -> 11px with a darker
      colour range and a per-cell `aria-label`, hidden chart/state summary
      text.
- [x] Console 1.6: chat / engine output / training are now tabs, state
      vector and raw log are collapsed `<details>`, chart + metrics stay
      visible.
- [x] 2.1: `POST /api/models/upload` — streamed to a temp file with a
      64 MiB cap, header validated against the same rules as the C loader,
      renamed into `models/uploaded/` only when valid.
- [x] 2.2: SAF import in the Android app (`OpenDocument`, `*/*`), streamed
      copy into `filesDir` via a `.tmp` file + atomic rename, same header
      validation, demo model kept as fallback, no persistable URI grant.
- [x] 2.3: JNI callback is now `onTokens(IntArray, done)` with 16-token
      batches and exactly one `done=true` call; all tokens are logged
      instead of the first 8.
- [x] Server binds to 127.0.0.1 unless `HYDRA_ALLOW_REMOTE=1`.
- [x] 3.3: `tools/server_test.js` (node:test, 15 cases) covering listing,
      bad input, path escapes, prompt plumbing, upload accept/reject and
      the loopback-only binding. Wired into `lint.yml` (`web-lint`).
- [x] CI: a new `build-test` step proves the prompt reaches the engine and
      rejects malformed prompts; negative control (prefill turned into a
      no-op) verified to fail.
- [x] Local verification: `make test` 56/0, ARM/NEON 62/0 under qemu,
      ESLint clean, `node --test tools/server_test.js` 15/15,
      `jni_signature_check.sh` green with 3 working negative controls,
      gradle `assembleDebug assembleRelease lintDebug` green with
      "0 errors, 0 warnings".
- [ ] PR + merge — **user approval needed** (R14).

## Round 2026-10-02 (GPU feasibility: measure, optimise the CPU, honest PoC)

Audit first: Phase 1.3 (JNI batching), SAF import, the `labs()` sign fix
and the APK-in-release workflow were **already merged** in PR #26/#24 —
recorded in `docs/gpu-feasibility.md` so nobody repeats them.

- [x] rules.md: **R31** (every change through a PR, English artefacts) and
      **R32** (measure before claiming; say "blocked" when a number cannot
      be taken)
- [x] Phase 1.1 — layer aggregation `A[i] = sum_l w1`, `B[i] = sum_l w2`
      computed once at load. Equivalence against a **reference
      implementation inside the test file** (not the engine's own scalar
      branch): 4 seeds x {1,4,37} layers, identical tokens *and* state.
      Negative control: shifting `B[0]` by one makes it fail.
- [x] The NEON kernel was rewritten for the aggregated loop instead of
      being left dead — otherwise the ARM path would have silently fallen
      back to scalar while its own tests still passed.
- [x] Phase 1.2 — `hydra_engine_step_fast()` + `--fast`, dimension 0 only.
      Bit-identical token streams proven over 3 models x 32 tokens, plus a
      negative control.
- [x] Phase 0 — `--bench` in the CLI (JSON, warmup, MONOTONIC) and
      `HydraBridge.benchmark()` in JNI measuring native-only vs per-token
      callback vs batched callback. Measured here: **132–149 ns/token full,
      18–19 ns/token fast, ~7.2x**, unchanged at 256 layers.
- [x] Crash handler in all three applications: window errors and unhandled
      rejections in the console, uncaught exceptions in the Node server
      (`GET /api/errors`, polled by the UI), and a default
      `UncaughtExceptionHandler` on Android writing `last-crash.txt` and
      showing `CrashActivity` with a clipboard copy button.
- [x] Permissions gate in `ci.yml`: the APK must not request any dangerous
      permission. SAF needs none; verified locally with `aapt`.
- [x] Phase 2 — `tools/gpu_bench/`: GLES 3.1 compute benchmark, CPU
      reference + GPU path + K sweep, checksum gate. **Compiles for arm64
      with the NDK, never executed** — no device, and no `glslangValidator`
      for the shader, so the `.spv` is deliberately not committed.
- [x] `docs/gpu-feasibility.md`: measured vs blocked separated, conclusion
      "do not start v2 yet" with the reasoning.
- [x] UI: focus-visible rings, `prefers-reduced-motion`, empty state for
      the engine panel, 44px touch targets on small screens.
- [x] Verified: `make test` 61/0, ARM/NEON 67/0 under qemu, gcc+clang
      `-Werror`, cppcheck, ASan+UBSan 61/0, ESLint clean, `node --test`
      18/18, trainer test, JNI gate (2 native methods), gradle
      `assembleDebug assembleRelease lintDebug` = *No issues found*.
- [ ] PR + merge — **user approval needed** (R14)
