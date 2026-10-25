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

**2026-10-25 — Implementation round: Min-P sampling & repetition penalty,
AVX2 x86-64 kernel, transformer primitives (RMSNorm/RoPE/SwiGLU),
streaming ring-buffer KV cache with attention sinks, safetensors import
with absmean ternarization (spec M1–M6 + verification V).**

Branch `arena/02477850-bestai`, base = main @ dccac28 (post PR #49 merge).
Invariants: zero fake checkboxes, zero stubs, baseline
`[471, 386, 385, 386, 385, 386, 385, 386]` (start_token=42, 8 steps)
untouched, 100% English artefacts, every change logged here and in
CHANGELOG.md. (No native todo tool in this session — this section is the
tracker, per the established pattern.)

### Plan (2026-10-25, implementation round)

- [x] **M1 Hygiene.** `CLAUDE.md` bridge (`@AGENTS.md` first line);
      verify `.gitignore` covers `.agent_memory/` + `RESEARCH.md`;
      architectural audit + derivations into `.agent_memory/RESEARCH.md`
      (+ Notion "AI Notizen" sync, connector active); `git rm`
      `hydra_stone_model_engine.zip` (grep-verified: referenced nowhere).
      DONE: CLAUDE.md + RESEARCH.md created, zip `git rm`ed; Notion sync
      at round close. License untouched (legal decision, documented).
- [x] **M2 Sampling.** `hydra_sampler_config_t` + `hydra_sample_token()`
      (include/hydra_model.h, src/hydra_engine.c); repetition penalty over
      a token window; numerically stable softmax; Min-P filter vs p_max;
      seeded xorshift64*; greedy guard (temp<=0 or min_p>=1) = native
      engine step (baseline preserved bit-for-bit); CLI flags
      `--temp`/`--min-p`/`--rep-penalty`; tests: penalty breaks a
      2-token oscillation, min-p filters the tail, greedy == native,
      hand-computed seeded sample.
- [x] **M3 AVX2.** `src/hydra_simd.h`: `#ifdef __AVX2__` step-time kernel
      (8 dims per 256-bit lane, int32 math provably exact under the layer
      cap, widen to int64) + load-time v2-plane unpack kernel
      (`_mm256_shuffle_epi8` LUT); runtime CPU detection with scalar
      fallback in `hydra_engine_step()`; Makefile detects x86-64 +
      compiler `-mavx2` support; parity tests (AVX2 kernel vs scalar
      reference, random + edge-case matrices) guarded `#ifdef __AVX2__`
      like the NEON tests.
- [x] **M4 Transformer primitives.** `hydra_rmsnorm`, `hydra_rope_inplace`,
      `hydra_swiglu` (zero-heap, NaN/Inf guards) + `hydra_kv_cache_t`
      streaming ring buffer with permanently retained attention sinks,
      O(1) in sequence length; unit tests: hand-computed values +
      eviction with sinks preserved.
- [x] **M5 Safetensors import.** `tools/safetensors_to_hydra.py`
      (stdlib only): 8-byte LE header length + JSON header with offset
      validation, F32/F16/BF16 decoding, absmean scale,
      RoundClip ternarization, **first shipped writer for header
      version 2** (4 weights/byte); round-trip test inside
      tools/gguf_test.py (synthetic fixture, independent re-read of the
      packed bytes, engine run vs reference, negative controls).
- [ ] **M6 Docs + baseline.** Roadmap checkboxes ONLY for the fully
      implemented+tested items (min-P, AVX2, safetensors import,
      streaming KV cache) with exact function references; "Full
      transformer forward pass" stays UNCHECKED (primitives + KV cache
      exist, no full pass — zero-fake-checkbox invariant);
      docs/ARCHITECTURE.md + docs/FORMAT.md (v2 writer note) +
      docs/GGUF-IMPORT.md if affected; CHANGELOG + this file;
      tools/test_count_baseline.txt updated in the same commit (R21).
- [ ] **V Verification.** make clean && make && make test; all
      python/node tools; all shell guardrail scripts; npm run lint;
      clean git status; CI (CI + Lint & Security) green on the PR.

### Design decisions (recorded before coding, per R26)

1. **M2 logit definition (honest, no invention):** the engine has no
   per-vocabulary logits — decode is `acc[0] -> token` (one number).
   Therefore: greedy mode (temperature <= 0 OR min_p >= 1) calls the
   native `hydra_engine_step()` — bit-identical to today, which is what
   keeps the baseline sequence untouched. Sampling mode defines the
   candidate logit as the accumulator the engine WOULD compute if the
   candidate c were fed: `L(c) = A[0]*c + B[0]*state[0]` (the same
   acc[0] driving state and decode); repetition penalty adjusts L(c)
   for c in the recent window; stable softmax with 1/temperature;
   min-p filter keeps P(c) >= min_p * p_max, renormalizes; seeded
   xorshift64* picks; the state then advances with `step(c*)` (seed
   invariant: next seed = last emitted token, exactly like greedy).
2. **M3 exactness:** int32 intermediates in the AVX2 kernel are
   provably exact: |A|,|B| <= layers <= 4096, |token| <= 1023,
   |state| <= 127 -> |A*t + B*s| <= 4096*1150 = 4,710,400 < INT32_MAX
   (same bound the NEON kernel documents); widen to int64 afterwards.
   Bit-identical to the scalar loop by construction (same integer
   ops); parity tests are the proof, kernel-level like test_neon_*.
3. **M5 pipeline** mirrors tools/gguf_to_hydra.py (A/B tensor
   selection, gamma = 1/mean|W|, threshold mean/2, fold-to-dim,
   normalise to peak 127, layers = peak) so both importers produce
   comparable models; the ONLY difference is the output packing
   (v2, 4 weights/byte) and the container being .safetensors.

### M1+M2 results & errors (2026-10-25, logged per R2/R3)

**Measured:** `./hydra-test` 113/0 (x86, was 81; +32 sampler checks,
both architectures per tools/test_count_baseline.txt); baseline run
`[471, 386, 385, 386, 385, 386, 385, 386]` @ 42/8 UNCHANGED;
`jni-batch-test` 86/0, `large-model-test` 7/0, `gguf_test.py` 40/0;
count check x86 ok at 113.

**Errors found & fixed (all in this session, per R3):**

1. **Segfault (exit 139) in the new test suite.** `static const
   uint16_t two_by_63 = {63, 63};` compiles as a SCALAR in C (second
   initializer element ignored, warning only) — passing that scalar
   where a `const uint16_t *` is expected turns the value 63 into a
   pointer. Lesson: array initializers in C need the `[2]` declaration;
   the "warning" here was actually the crash.
2. **`(double *)&sampler.temperature` near-miss in main.c.** Writing a
   double through a reinterpreted float* corrupts the next struct
   field and misencodes the value. Caught in self-review before the
   first build; fixed with a local `double v` temp.
3. **Prefill/step token mod:** `hydra_engine_step()` mods
   `token_in % vocab`, so a prefill token >= vocab does NOT drive the
   state the way its face value suggests — the first negative-logit
   test design (prefill [10,5] for state 10) silently produced state
   2. Re-derived with the mod included: prefill [7,0,7,0] on the
   (A0=1,B0=−1) model drives state 14 via s' = t − s.
4. **2-bit ternary codes are NOT nibbles.** v1 packing uses
   `packed & 0x03` (w1) and `(packed >> 2) & 0x03` (w2) — byte 0x21
   decodes to w2=0, not w2=−1. The (w1=+1, w2=−1) byte is 0x09.
   Caught when the debug model printed `B0=0` instead of −1.
5. **Test-local logic bugs:** (a) the distinct-value counter loop
   `for (i=0; i<8 && seen[i]; ++i)` can never enter while `seen[]` is
   empty; replaced with a bitmask popcount. (b) `cfg.min_p` was left
   at 0.5 from the previous block inside the min_p=0.1 seed loop.
   (c) popcount via `((mask&1)+(mask&2)+(mask&4))` is wrong (a single
   high bit already sums >= 2) — use `(mask&b) != 0` per bit.

**Design notes that bit the tests (kept for M3/M5):** the engine's
decode `out = (acc0 mod V + c + 1) mod V` is injective in the chosen
candidate c only for ODD V (or A0+1 coprime to V) — min-P/CDF tests
therefore use V=65/V=5, and the penalty tests verify the (A0,B0,s0)
combination makes the two candidate outputs distinguishable.

### M3 results & errors (2026-10-25, logged per R2/R3)

**Measured:** `./hydra-test` 121/0 (x86 AVX2 build; +8 AVX2-only
checks); baseline `[471,386,385,386,385,386,385,386]` @ 42/8
UNCHANGED on the AVX2 build (the SIMD step path is live — end-to-end
parity); jni-batch 86/0, large-model 7/0, gguf_test 40/0; zero
warnings; count check ok at 121. Baseline file: x86 113→121, arm
119 unchanged (AVX2 checks compile only into -mavx2 builds).

**Errors found & fixed (R3) — the SIMD revision was NOT bit-parity on
the first pass; the parity test caught all four of these:**

1. **256-bit lane width:** a __m256i holds 8 int32, not 16 — the first
   step kernel left dims 4-7 (and 12-15) unwritten. Fixed at 8 dims
   per iteration with a correct 4+4 → 8 int64 widening.
2. **v2 unpack element count + layout:** a 256-bit lane = 32 bytes =
   128 weights, not 32 dims; and the original "sum the four weight
   vectors" collapsed 4 dimensions into one (horizontal add instead of
   per-dim values). Rewritten at 16 bytes = 64 dims with the correct
   scatter: dim 4k+j gets exactly weight j of byte k.
3. **`_mm_shuffle_epi8` operand order:** the TABLE is the FIRST
   argument, the index the second — the first revision looked up
   `bytes[table[i]]` instead of `table[bytes[i]]` (raw bytes came out
   of the "LUT").
4. **VPSHUFB bit-7 zeroing:** any index byte with bit 7 set is
   zeroed. The 16-bit lane shifts leak the next byte's low bits into
   bits 6-7 of the index (0x5555 → 0x...8A), so ALL four indices need
   the `& 0x0F` mask — the code lives in 2 bits and the table absorbs
   bits 2-3. Symptom: code-10 (−1) lanes randomly became 0.
5. **(compile) `const` lazy init** in the header LUT is not legal C
   (function-scope `static const uint8_t T[256]` written once) →
   non-const `static uint8_t T[16]` + ready flag.

**Design notes kept for M5:** the AVX2 v2 unpack is covered
kernel-level (random + all-code sweep vs an independent scalar
reference). The first END-TO-END v2 dim-64 model (file → load → AVX2
unpack → step) appears with M5, when `safetensors_to_hydra.py`
becomes the first shipped v2 writer — its gguf_test.py round-trip
will run on this AVX2 build and prove the path end-to-end.

### M4 results & errors (2026-10-25, logged per R2/R3)

**Measured:** `./hydra-test` 142/0 (zero warnings; +21 checks, all
architectures); baseline `[471,386,385,386,385,386,385,386]` @ 42/8
unchanged; jni-batch 86/0, large-model 7/0, gguf_test 40/0. Baseline
file: x86 121→142, arm 119→140.

**Error found & fixed (R3):** the KV-cache test itself had a bug —
`(float)-expect[i]` with `expect` declared `uint32_t` negates in
unsigned arithmetic (−1 → 4294967295), so the comparison failed from
i=1 on while the engine was correct (proven by instrumenting
`hydra_kv_cache_get`: slots, sinks, and buffer contents were all
right). Always negate after casting to a signed/float type:
`-(float)expect[i]`. Same trap fixed in 4 places.

**Design notes:** the M4 primitives are float/zero-heap and
R14-guarded (finiteness checked before any store). The KV cache
layout: appends t < S go to permanent sink slots; appends t ≥ S go to
window slot `S + (t−S) mod (C−S)`; `get(i)` maps sinks first, then
window tokens with oldest resident append = `max(S, n−(C−S))`. The
first CONSUMERS of these primitives appear in M5/M6 (a transformer
head over the ternary engine), which keeps M4 a self-contained,
tested building-block layer.

### M5 results & errors (2026-10-25, logged per R2/R3)

**Measured:** `gguf_test.py` 40 -> **59 checks, 0 failed** on the x86
AVX2 build with `--engine ./hydra-run` (19 new safetensors checks:
converter acceptance, v2 header shape, independent byte-reader A/B
parity, independent quantisation oracle, F32/F16/BF16 equivalence, the
real C engine on two (token, steps) pairs, six negative controls,
different-source-different-model). The C test count is unchanged (142
x86 / 140 arm), so `tools/test_count_baseline.txt` needed no edit.
Baseline `[471,386,385,386,385,386,385,386]` @ 42/8 untouched (the
starter model is v1; the v1 path is byte-identical).

**Error found & fixed (R3):** `_st_fails` in the new gate originally
wrote its argument as bytes unconditionally, so the path-based negative
controls (truncated / past-end files) crashed the gate with
`TypeError: a bytes-like object is required, not 'str'` instead of
testing the converter. Fixed by accepting either raw bytes or an
existing path (mirroring `_convert_fails`). Caught immediately on the
first run - the gate is its own negative control.

**Design notes:** the converter never materialises the ternary matrix -
`W_q = clip(round(W/(gamma+eps)))` is a threshold, so the ternary column
sums are `count(W >= s/2) - count(W <= -s/2)`. The v2 writer stores the
folded column sums as layer sums of +/-1 (the same exactness trick as
the v1 GGUF converter), so `sum_l w1[l][i] == A[i]` by construction.
The first shipped WRITER for the v2 format is this tool; the engine's
v2 reader (and, on AVX2 builds, the AVX2 unpack kernel) has now had its
first end-to-end file-level exercise.

## Previous task (2026-10-25 audit — completed, merged as PR #47/#48/#49)

**2026-10-25 — Full-repository audit, Claude-skills adherence, 6-milestone
deepening run (adversarial verification, zero deletions).** User instruction:
analyse and document everything, follow every `.claude/skills/*` rule, find
bugs or "lies" (unverifiable claims), never delete what works, and make the
whole verification matrix pass. Artefacts in English (R31); conversation may
stay German. Milestones M1–M6, verification V.

### Plan (2026-10-25)

- [x] **M0. Baseline (before any change).** All gates run and recorded:
      `make all && make test` builds clean; `./hydra-test` 71/0 (x86);
      `./jni-batch-test` 86/0; `./large-model-test` 7/0;
      `python3 tools/gguf_test.py --engine ./hydra-run` 38/0;
      `node --test tools/server_test.js` 37/0; console-train, contrast,
      string-format, all bash guard scripts green. Environment repairs:
      pyyaml installed (PEP 668 -> `--break-system-packages` in sandbox),
      llama.cpp submodule initialised at the pinned v0.5.0 commit
      (`llama_pin_check` 5/0, negative test 7 proved,
      `ci_release_version_test` green). `jni_signature_check` SKIPs without
      javac; ARM/NEON parity gate **blocked, not measured** (no aarch64
      cross toolchain or qemu in this container) — stated, never hidden (R32).
- [x] **M1.** File inventory + live audit of GitHub Issues #46/#44 +
      git-ignored agent brain (`.agent_memory/`, `RESEARCH.md`).
      DONE: full inventory in `RESEARCH.md` §1 (timestamped); #46 audited
      (scorecard claims unverified -> F-6; "~1.06 KiB" disproven by
      measurement -> F-15); #44 audited (random-LUT proposal rejected per
      R5/R6 -> F-5, nothing implemented); `.gitignore` now excludes
      `.agent_memory/` and `RESEARCH.md` (verified via `git status`).
- [x] **M2.** Canonical root `AGENTS.md` + Notion "AI Notizen" documentation
      (local mirror `.agent_memory/NOTION_AI_NOTES.md`).
      DONE: `AGENTS.md` committed (architecture map, skill bindings, exact
      verification commands, doc-location table). Notion page created
      ("📝 AI Notizen — Hydra-Stone (BestAI) Technical Documentation",
      under the "AI-TRAINING" parent — top-level page creation is not
      possible via the API); content filled at M6 close; local mirror
      `.agent_memory/NOTION_AI_NOTES.md` written the same day (git-ignored).
- [x] **M3.** Mathematical audit of the GGUF -> .hydra conversion
      (absmean gamma, ternary projection, packing, shapes, vocab metadata);
      fix only what is actually wrong; proof in `RESEARCH.md` and
      `docs/GGUF-IMPORT.md`.
      DONE: threshold `mean*0.5` proven the reference rule (RESEARCH.md §4,
      literature: b1.58 absmean, CAT-Q, TernaryLM); converter fixed (F-3:
      `pick_vocab` dropped the vocab count in the report — fixed + 2
      regression checks); the v2 (header version 2) loader path existed
      undocumented/untested (F-1) — now implemented guards (`dim % 4` -> -8,
      version-aware `weights_len` bounds) + 10 new checks in
      `test_version_two_format` (falsifiability proved: corrupting one packed
      bit fails exactly 4 checks); trainer pair search implemented (F-4)
      with A/B evidence (RESEARCH.md §5: official corpus 18.2% -> 9.1%,
      alt3 27.3% -> 36.4% proving the lockup break). `gguf_test.py` 40/0.
- [x] **M4.** C engine / JNI / dual-engine hardening audit (leaks, file
      descriptors, overflow, JNI local refs, thread safety, gpu_bench
      consistency).
      DONE: NO code changes needed — `hydra_jni.c` clean (fd discipline,
      local-ref management, `DeleteLocalRef` in loops); `llama_jni.cpp`
      clean (`token_to_piece(…, true)` special-flag matches pinned v0.5.0
      examples, verified against the submodule source); `gpu_bench` is
      exemplary (NOT MEASURED separation, checksum gate); overflow PoC
      already covered by `test_large_model.c` (7/0). All findings recorded
      in RESEARCH.md; nothing "fixed" that was not broken (no deletions,
      no churn).
- [x] **M5.** Android UI/UX: active-engine status, tok/s telemetry, RAM
      display, atomic-switch ergonomics; `android_autostart_check`,
      `android_crash_check` and `contrast_check` must stay green.
      DONE: bug found and fixed — `HydraEngine.kt` lost the cancel state
      (second summary built from a phantom `"tokens"` key; cancelled runs
      reported as errors) -> single completion summary from the native
      JSON (key `"emitted"`), cancelled => no error (F-14). UI now always
      shows the active engine (`.hydra` vs `.gguf`), live tok/s + latency +
      RAM footprint (`EngineStatusRow`/`RunTelemetryRow` in `HydraApp.kt`,
      `RunStats` updated per token in `MainActivity.onToken`), model detail
      lines (llama: quant/weights/state as reported by llama.cpp itself;
      hydra: format/weights/vocab), failed loads logged, stale detail
      cleared on import-restore failure. Gates: autostart 31/0,
      autostart-negative 14 proved, crash 25, header 22, contrast 17 pairs
      — all green. Kotlin NOT compiled in this sandbox (no NDK egress):
      stated as blocked, never claimed verified (R32).
- [x] **M6.** Docs + `CHANGELOG.md` + session sync (English, R31); README
      audited against `docs_claim_check.sh`.
      DONE: README density table + "honest framing" paragraph + v1
      footprint line + roadmap item corrected (F-16: the "2 bits/weight"
      claim was wrong — v1 is 4 bits of file per weight, v2 2 bits;
      corrected in README.md, docs/FORMAT.md, docs/GGUF-IMPORT.md,
      docs/ARCHITECTURE.md, server.js comment); "~1.06 KiB" memory claim
      replaced with the measured 656 B context / 576 B step data (F-15);
      `CHANGELOG.md` created; this file closed out below; `errors.md`
      appended (env gaps + the F-14 class + the F-16 class); RESEARCH.md
      gained F-14/F-15/F-16; `docs_claim_check.sh` green.
- [x] **V.** Full verification matrix re-run until 100% green; git hygiene:
      `RESEARCH.md` and `.agent_memory/` ignored, tracked changes clean.
      DONE (see "Final verification (V)" table at the end of this section).
      PR opened from `arena/4a16d961-bestai`; NOT merged — merge is the
      user's explicit decision (R17).

### Final verification (V), 2026-10-25, x86-64 sandbox, post-M6

| Gate | Result | Note |
|---|---|---|
| `make clean && make && make test` | PASS | build clean |
| `./hydra-test` | **81 / 0** | baseline updated 71 -> 81 (+10 v2 checks), x86 |
| `tools/test_count_check.sh` | PASS | "81 matches baseline" (R21: parsed, not hardcoded) |
| `./jni-batch-test` | 86 / 0 | |
| `./large-model-test` | 7 / 0 | overflow PoC |
| `python3 tools/gguf_test.py --engine ./hydra-run` | **40 / 0** | (+2 vocab-report checks) |
| `python3 tools/gguf_speed.py` | PASS | benchmark runs, report printed |
| `node --test tools/server_test.js` | 37 / 0 | |
| `node tools/console_train_test.js` | PASS | 0.0% -> 9.1%, 80 moves |
| `node tools/contrast_check.js` | 17 pairs, WCAG AA | |
| `python3 tools/string_format_check.py` | ok (37 entries) | |
| `tools/jni_signature_check.sh` | SKIP | no `javac` in sandbox (stated, R32) |
| `tools/android_header_check.sh` | 22 / 0 | |
| `tools/android_autostart_check.sh` | 31 / 0 | |
| `tools/android_autostart_negative_test.sh` | 14 proved | |
| `tools/android_crash_check.sh` | 25 / 0 | |
| `tools/llama_pin_check.sh` | 5 / 0 | |
| `tools/llama_pin_negative_test.sh` | 7 proved | |
| `tools/docs_claim_check.sh` | ok | after F-15/F-16 corrections |
| `npm run lint` | clean | |
| ARM/NEON parity, APK build, device run | **blocked, not measured** | no aarch64 cross toolchain / qemu / NDK egress in this container; CI legs re-measure (arm baseline 87 derived, CI re-measures) |

Git hygiene: `git status` shows only tracked changes; `RESEARCH.md` and
`.agent_memory/` are ignored (proven by `git check-ignore`).

### Commits of this run (branch `arena/4a16d961-bestai`, base 46f2531)

- `1eaef54` — M1: `.gitignore` += `.agent_memory/`, `RESEARCH.md`; `RESEARCH.md` created (git-ignored): inventory, baseline, findings F-1..F-13, ternary research, issue audit.
- `202fef6` — M3: v2 guard + version-aware bounds in `src/hydra_engine.c`; +10 checks `test_version_two_format`; `gguf_to_hydra.py` pick_vocab fix + 2 regression checks; `hydra_train.js` pair search; FORMAT.md "Version 2" + disambiguation + threshold proof.
- `c477a52` — M5: `HydraEngine.kt` cancel-state fix; `HydraApp.kt`/`MainActivity.kt` engine status, live telemetry, model detail; `strings.xml` +4.
- (M6 commit) — README/FORMAT/GGUF-IMPORT/ARCHITECTURE/server.js density corrections (F-15/F-16), `CHANGELOG.md`, session close-out, `errors.md` append, RESEARCH.md F-14..F-16.

### CI fix round (2026-10-25, after the PR opened)

- [x] CI failed on `compileDebugKotlin`: `MainActivity.kt:987:57` — the
      M5 `selectEngine()` edit called `file.absolutePath` on a nullable
      `File?` (the compiler cannot infer `result.ok ⇒ file != null`).
      Fixed with an early return for the no-model case (same UI state +
      log line as before); the rest of the function now runs on a
      smart-cast non-null `file`. Not compilable in the sandbox (no
      kotlinc) — verified by reading; CI is the compile proof.
- [x] User-reported 666 permissions: 5 of 12 gate scripts were committed
      100644 despite `#!/usr/bin/env bash` (on a umask-000 checkout they
      show up as `-rw-rw-rw-` and cannot run directly). All five are now
      100755 in git; every shipped shebang script has the exec bit.
- [x] CI `dist/` upload error was the downstream of the Kotlin failure
      (the APK-inspection step creates `dist/`); no workflow change.
- [x] Committed and pushed; PR #47 updated.

### Round 2026-10-25 (post-merge: CI red on lint)

PR #47 merged by the user at 18:31 (23baf2b). Post-merge CI: **CI green**
(Kotlin fix verified — full gradle build + APK verification passed on
both the PR head and main), but **Lint & Security** and **Release**
failed on the Android-lint gate. Job logs/artifacts are unreachable from
this sandbox (results-receiver + Azure blob not in egress), so the
findings were inferred from the diff and the repo's own conventions:
three M5 telemetry strings were plain `<string>` resources with `%1$d
tokens` — a quantity — while every existing quantity string in the repo
is a `<plurals>` (log_size, log_running, log_bench_start), and the gate
fails on ANY finding. Fixed by converting to `<plurals>` (one/other,
matching the existing style) and updating the three `stringResource`
call sites in HydraApp.kt. Committed on the new session branch
`arena/6bf4fc8e-bestai`, pushed, **PR #48** opened (R17: merge is the
user's).

**CI round 1 (db85ccd):** CI green (7m32s), Lint & Security red — with a
different finding than inferred: **"3 errors, 0 warnings"**. The
PluralsCandidate warnings are gone (conversion was right), but each
`stringResource(R.plurals…, …)` call raised ERROR `ResourceType`
("Expected resource of type string") at HydraApp.kt:329/331/333: in this
Compose version (BOM 2024.09.03) overload resolution picks the
`stringResource(@StringRes Int, vararg Any)` overload, and lint rejects
an R.plurals id at a @StringRes parameter. The report itself was
unreadable from this sandbox (log/artifact hosts outside egress) — the
user pasted it, and asked to make such reports retrievable going forward.

**Round 2 (this commit):** (a) call sites switched to
`LocalResources.current.getQuantityString(R.plurals…, …)` — one
unambiguous signature, @PluralsRes parameter, same pattern MainActivity
already uses; the strings stay plurals. (b) `lint.yml` now mirrors the
full report on failure as a **PR comment** (pull-request runs) and
**step summary** (always) — the report lands where API-only clients can
read it; an in-sandbox fetch script is impossible (egress blocks the log
hosts), so CI-side mirroring is the workable form of the user's request.
errors.md: overload trap + two-round principle; CHANGELOG updated.
Committed as 199221c on top of db85ccd (push).

**CI round 2 (199221c):** Lint & Security red — but EARLIER than
expected: the `gradle lintDebug` step itself (which sits before the
report step) aborted, so the publishing step was skipped and the report
was again UI-only (no PR comment). The actual findings of 199221c are
still unknown — the tree is verified consistent (plurals in
strings.xml + getQuantityString call sites), so either the overload fix
was insufficient or a different error-level finding appeared. Side
finding: the sandbox snapshot had restored strings.xml to the pre-db85ccd
version while HydraApp.kt kept the db85ccd state; a blind commit would
have shipped a dangling R.plurals reference (compile error). Caught,
recovered with `git reset --soft` + `git restore --source=HEAD`; the
pushed 199221c contains the correct plurals.

**Round 3 (this commit):** workflow restructure in **lint.yml AND
release.yml**: (1) the `gradle lintDebug` step is no longer fatal
(`|| echo` — gradle writes the report before aborting anyway); (2) the
check step verifies the report exists, and on findings publishes step
summary + PR comment (PR number from the event, or resolved from the
commit SHA via `/commits/{sha}/pulls`, which also covers push runs that
update a PR) and fails the job itself. YAML validated. Committed as
b53d738, pushed.

**CI round 3 (b53d738):** Lint & Security rot — aber der neue Ablauf
lief: gradle-Step success (non-fatal), Check-Step failure (Gate als
einziger Failure-Punkt ok). PR-Comment fehlte trotzdem: lint.yml hat
`permissions: contents: read` auf Workflow-Ebene → issues-API-Call
403, vom `|| true` verschluckt (genau die Falle, die der
Fallback-Dämpfer abfangen soll, deckt jetzt die Funktion selbst).
Der CI-Run auf 199221c (CI workflow) war ebenfalls rot — Code-Tree
ist verifiziert konsistent, also entweder transiente Infra oder ein
echter Compile-Fehler; der b53d738-Run klärt das.

**Runde 4 (jetzt):** job-separierte Permissions `issues: write`
(überschreibt das Workflow-Level, scoped auf die zwei Android-Jobs)
+ `::warning::`-Marker auf dem Comment-Fallback-Pfad (eine gebrochene
Spiegelung wird dann in den Annotations sichtbar). errors.md:
Permission-403-Eintrag. Nächster fehlgeschlagener Run postet den
Report als PR-Comment → tatsächliche Lint-Funde lesen → definitorischer
Fix. Commit 05dbbfe, gepusht. Nebenbefund aus der Analyse: CI (ci.yml)
ist auf 199221c UND b53d738 am Gradle-Build-Step rot (db85ccd war
grün) — gleicher Code in beiden → verdächtigster Kandidat ist der
HydraApp.kt-Änderung von Runde 2, aber ein Compile-Fehler vs.
Infrastrukturfehler war aus der Sandbox nicht unterscheidbar (Logs
UI-only).

**Runde 5 (jetzt):** schließt die letzte Diagnose-Lücke:
"Report fehlt" ist eine zweite Fehlerklasse (gradle stirbt vor der
Lint-Analyse, z.B. bei Kotlin-Compile-Fehlern → keine Report-Datei).
Die gradle-Steps in **ci.yml und lint.yml** loggen jetzt die volle
Ausgabe in eine Datei; der Check-/Publish-Step baut das Payload aus
der jeweils eingetretenen Klasse (Lint-Report ODER Gradle-Log-Tail
150–200 Zeilen) und publiziert es in beiden Fällen (Step-Summary +
PR-Comment). Der CI-APK-Build-Job bekommt dasselbe (Capture +
Publish + job-separierte issues:write). YAML validiert (3 Dateien).
Ab dem nächsten fehlgeschlagenen Run ist jeder Android-Fehler per API
lesbar — egal welche Fehlerklasse. Commit 9608e4d, gepusht.
Nebenbefund: der 9608e4d-Lint-Job lief nur 46 Sekunden und starb vor
der Lint-Analyse (kein Report) — der Footprint eines Compile-Fehlers.

**Runde 6 (jetzt):** WURZELURACHE: `androidx.compose.ui.platform.
LocalResources` existiert in dieser Compose-Version (BOM 2024.09.03)
NICHT — die Runde-2-Reparatur hat eine Library-API referenziert, die
es nicht gibt (ungedruckte Annahme, nicht einmal im Repo verwendet;
Theme.kt nutzt LocalContext). Ein Compile-Fehler erklärt alles
Gleichzeitig: CI-Build-Step rot (199221c + b53d738, gleicher Code),
Lint-Job "kein Report" (stirbt vor der Analyse), 46-Sekunden-Job.
Fix: `import androidx.compose.ui.platform.LocalContext` +
`val resources = LocalContext.current.resources` — dieselbe
Resources-Klasse, kanonischer Compose-Zugriff, API die das Repo schon
kompiliert. Sonst nichts geändert (Plurals, Call-Stil, Workflow).
Fehler-Artifact in lint.yml bekommt zusätzlich /tmp/lint-report.md +
gradle-lint.log. Doku: errors.md (nicht-existierende API +
Fehler-Fingerprint "CI rot + Lint ohne Report = Compiler, nicht
Lint-Finding"), CHANGELOG + session.md. Ergebnis: **CI und Lint &
Security grün auf a983414**; Nutzer hat **PR #48 merged** (20:04Z,
Merge-Commit afae8c8 auf main). Release auf main lief danach neu und
ist grün.

### Round 2026-10-25 (post-merge: Doc-Audit aus Screenshot)

Nutzer hat den GitHub-README-Stand (main, afae8c8) als Screenshot
gefordert, alles zu prüfen: Screenshot-Ansprüche + finale Doku in
jedem Ordner. Neue Session-Branch `arena/c19193e9-bestai` (von
afae8c8).

**Geprüft und bestätigt (gemessen/gestimmt, keine Änderung):**
Token-Sequenz `[471, 386, 385, 386, 385, 386, 385, 386]` (starter,
start_token=42, 8 Steps) per echtem Build+Run reproduziert; CI
neon-test (aarch64-Crosscompile + qemu, Paritäts-Run 42 8 --json) +
NEON-vs-Skalar im selben ARM-Binary (test_engine.c); Emulator-Evidence
in android/README.md; make_model.py assertet A[0] != 0 / B[0] != 0;
zero runtime npm deps + ESLint dev-only; `make ui` → :8787
(server.js PORT); FORMAT.md "Version 2" + test_version_two_format;
.hydra2 unimplementiert (kein hydra2 in src/include), FORMAT-V2.md
ist die Spec; docs_claim_check.sh grün.

**Gefunden und gefixt:** (1) Lizenz-Widerspruch — LICENSE ist
GPL-3.0-Volltext (GitHub-Header zeigt "GPL-3.0 license"), README
sagte "MIT" an zwei Stellen (License-Sektion + Struktur-Baum),
package.json "license": "MIT" → alle drei jetzt GPL-3.0 (LICENSE
selbst unangetastet — Rechtsentscheidung des Nutzers; die
llama.cpp-MIT-Referenzen in AGENTS.md/ANDROID-AUDIT.md sind korrekt
und bleiben). (2) Roadmap-Verweis auf eine "*Roadmap status*"-Sektion
in docs/ARCHITECTURE.md, die es nie gab (git log -S) → zeigt jetzt
auf die echte Sektion "Known Structural Redundancy", wo das
superseded-Design A[i]/B[i] steht (Zeile 106). Doku: CHANGELOG,
errors.md (Lizenz-/Cross-Reference-Fingerprint). PR geöffnet
(R17: Merge ist beim Nutzer).

### Session close (2026-10-25)

Everything the spec asked for is either done and verified or explicitly
marked blocked-with-reason (R32). No working code was deleted; no claim was
left unmeasured where a measurement was possible. Open items handed to the
user: (1) merge decision on the PR (R17); (2) `hydra_stone_model_engine.zip`
tracked but unreferenced — keep or remove (F-12); (3) issue #44 decision
(close as won't-fix per F-5, or scope a real task); (4) Notion page location
(created under "AI-TRAINING" because the API cannot create top-level pages —
user may move it).

### Superseded current task (2026-10-03, all steps done above it)

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
