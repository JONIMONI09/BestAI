# Session Archive — Hydra-Stone (hydra-stone)

Complete round log of `session.md` for everything BEFORE round 6
(2026-10-26, the .agents/ reorg). Nothing was deleted — the sections
moved here verbatim when session.md was slimmed (2026-10-26, round 7,
rules R33 context-budget finding: one long history file dilutes
attention and costs tokens on every load; the current context is the
summary tables + the open rounds only).

Contents: "Current task" (M1 completion round, merged as PR #55),
"Previous task" (2026-10-25 implementation round + Android
keyboard/CI; 2026-10-25 audit merged as PR #47/#48/#49; superseded
plan), "Plan", "Status / Notes", and the detailed round sections
Rounds 2026-10-02 through 2026-10-26 (round 5).

---

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
  `errors.md`, guide in `.agents/docs/ANDROID_SKILL.md`.
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
  "Done (earlier today)" and `.agents/docs/ANDROID_SKILL.md`.

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

**2026-10-26 — M1 completion: ARM NEON in-register v2 (2-bit) unpack
path (branch `arena/5774a639-bestai`, main @ 3cf4c85 post-PR-#50).**
User re-issued the 5-milestone spec (post-`dccac28` audit) and asked to
continue. No `todo_write` tool exists in this environment — milestones
are tracked here, `completed` only after build + tests pass.

**Audit of the spec's claims against current main (verified, not
assumed):**
- "2-bit weights still expanded to 8-bit int8_t in RAM during load" —
  **stale.** `hydra_build_aggregation()` decodes the packed bytes
  directly into the int32 A/B aggregation (scalar `…_v1_1` + the
  AVX2 LUT `hydra_avx2_unpack_v2_plane`); there is no expanded
  int8 weight buffer anywhere. The file is mmapped + MADV_SEQUENTIAL,
  measured RSS delta 0 after load (see the block comment in
  `src/hydra_engine.c`). Runtime resident weight memory is the 512
  bytes of agg_a/agg_b — below the spec's "4x less" target by design.
- "6 of 7 Roadmap items unimplemented" — **stale.** 5 of 7 done
  (min-p sampling, safetensors import, AVX2 kernel, v2 packing,
  ring-buffer KV). Remaining: full transformer forward pass (the
  primitives exist but are not wired — a separate roadmap item, not
  one of the spec's 5 milestones) and the `.hydra2` sparse-MoE
  container (explicitly "no part implemented").
- "hydra_stone_model_engine.zip still in root" — **gone** (M1 agent
  hygiene round).
- "CLAUDE.md still missing" — **exists** (M1 agent hygiene round).

**The one real M1 gap (this round's scope):** the spec's M1.2/M1.3
require a SIMD in-register unpack kernel in `src/hydra_neon.h`
alongside the AVX2 one; M1.4 requires bit-identical tests across
scalar/NEON/AVX2. Currently the NEON architecture aggregates v2 models
with the pure scalar loop — only the x86 build has a vectorised v2
decode.

**Plan (milestones, each completed only after its build + tests pass):**
- [x] M1a: `hydra_neon_unpack_v2_plane()` in `src/hydra_neon.h` —
      fully vectorised 2-bit→signed decode of a v2 plane (16 bytes =
      64 dims per chunk). FINAL FORM (after two review-caught design
      errors): weights normalized to the low 2 bits via logical right
      shifts (`vshlq_n_u8` negative immediates — a pure AND mask
      CANNOT work: `vqtbl1q_u8` indexes with the lane's LOW 4 bits
      only, so 0x3C/0xF0/0xC0 masks would decode planes 1-3 as 0),
      masked 0x03, decoded through the 16-entry `T[i]=ternary(i&3)`
      LUT (loaded once via `vld1q_u8` from an `aligned(16)` static —
      a raw array is not a `uint8x16_t` and needs 16-byte alignment),
      widened s8→s16→s32 (sign-extension carries −1=0xFF), 4-way
      interleave in TWO `vzipq_s32` rounds with the pairs two apart
      (q02=zip(r0,r2), q13=zip(r1,r3), re-zip groups instead of
      alternating — the first draft's vswpq attempt had the wrong
      lane order and a wrong m*8 store base; fixed to m*16),
      `vaddq_s32` into the aggregation. Bit-identical by
      construction: exactly one integer per dimension, same table.
      Math proven on x86 by an element-level shadow model
      (/tmp/neon_shadow.c, not committed): 200,000 random dim-64/128
      planes bit-identical to the scalar reference; intrinsic
      spellings verify in the CI qemu leg (no cross toolchain in the
      sandbox — no root for apt, no aarch64 gcc).
- [x] M1b: wired into `hydra_build_aggregation()`'s v2 branch
      (`use_neon = dim % 64 == 0` under `#if defined(HYDRA_NEON_V2_UNPACK)`
      — originally `__ARM_NEON`, tightened after the Android
      armeabi-v7a build failed: the kernel's `vqtbl1q_u8` is
      AArch64-only, so 32-bit ARM takes the scalar v2 decode;
      NO runtime probe — NEON is base ISA on AArch64, a build
      without NEON never defines the macro so the kernel is simply
      absent; per layer: 64-dim chunks for A+B, `continue`; scalar
      fallback for other dims). Makefile: `src/hydra_neon.h` added
      to the hydra-run/hydra-test/large-model-test dependencies
      (rebuild correctness on the CI ARM leg).
- [x] M1c: `tests/test_engine.c` under `#ifdef __ARM_NEON`:
      `test_neon_v2_unpack` (6 checks: random 3-layer dim-64 planes
      vs `ref_unpack_v2_plane`, 4-code sweep ×4 hand-pinned,
      dim-128 two-chunk) + `test_neon_v2_load_matches_scalar` (5
      checks: all-4-codes-present proof, load rc, agg_a/agg_b
      bit-exact vs in-test scalar decode of the same raw bytes,
      32-token stream vs the documented formula driven by the
      independent aggregates — the reference never sees the kernel).
      Arch-neutral helpers `wr_header_version` +
      `ref_unpack_v2_plane` moved ABOVE the NEON block (shared by
      NEON + AVX2 suites; no use-before-definition, no duplication).
      +11 ARM-only checks → baseline arm 140→151, x86 142 (updated
      in this commit, dated comment; ARM re-measured by the CI qemu
      leg).
- [x] M1d: docs + records — CHANGELOG.md entry, README (v2 packing
      roadmap line: three bit-identical load paths + parity gates;
      NEON bullet: unpack kernel added), this file,
      `.agent_memory/RESEARCH.md` (new: design, the two caught
      design errors, shadow proof, CI facts). Notion sync: done
      2026-10-26 in two steps (initial M1 record, then the close-out
      confirming CI green 19/19 + the four CI-driven fixes).
- [x] M1e: full verification matrix GREEN (x86 sandbox; NEON executes
      only in CI): make clean/make/test 0 warnings; hydra-test
      142/0; jni-batch 86/0; large-model 7/0; baseline
      `[471,386,385,386,385,386,385,386]` @ 42/8 UNCHANGED (v1 model
      never enters the v2 branch); gguf_test 59/0; gguf_speed ok;
      string_format_check 35/0/4; server_test ok;
      console_train_test pass; contrast_check 17 AA; all 10
      guardrail scripts PASS (llama_pin 5/0 + negative 7 proved —
      needed a submodule re-init, pre-existing empty-clone state);
      test_count_check ok (142, x86 baseline); release-version test
      all cases (via /tmp/yvenv PyYAML); npm run lint 0; git status
      clean apart from the 8 intended files.
- [x] M1f: push + PR (R17: user merges) + CI polling until the
      **ARM neon-test leg** (qemu-aarch64-static) is green — that
      leg is the only place the NEON kernel executes.

**M1 close-out (2026-10-26): CI fully green — 19/19 checks pass**
(neon-test, build-test ubuntu+macos, Android lint+APK, lint,
CodeQL ×3, ASan+UBSan, JNI ×2, Docs/GPU/Release/Semgrep/Web/
llama-pin). Four CI-driven fixes landed on top of the kernel commit
(all documented per-change in CHANGELOG.md):
1. clang-gate: `ref_unpack_v2_plane` gated by
   `__ARM_NEON || __AVX2__` (later `HYDRA_NEON_V2_UNPACK || __AVX2__`)
   — the x86 no-AVX2 clang syntax check tripped -Wunused-function
   (clang diagnoses it in analysis; gcc only in codegen).
2. Intrinsic portability (reproduced locally with a zig-bundled
   clang aarch64 toolchain): negative `vshlq_n_u8` immediates are
   rejected by the AArch64 headers → `vshrq_n_u8(2/4/6)` (identical
   semantics after the 0x03 mask); unsuffixed `vreinterpretq_s8`
   does not exist in AArch64 headers of gcc-12 OR clang →
   `vreinterpretq_s8_u8` (the one spelling both declare).
3. Test heap overflow caught by the new ASan diagnostic step (macOS
   unit-test leg): `test_neon_v2_load_matches_scalar` wrote
   layers×dim/4 bytes into a dim/4 allocation → macOS "Trace/BPT
   trap" + qemu/glibc "malloc(): corrupted top size". Fixed the
   allocation; all other test mallocs audited.
4. Android NDK compiles the engine for armeabi-v7a (32-bit ARM):
   `vqtbl1q_u8` is AArch64-only → new `HYDRA_NEON_V2_UNPACK` macro
   (AArch64-only) gates the v2 kernel, the engine wiring, and the
   two v2 tests; 32-bit ARM uses the scalar v2 decode at load
   (bit-identical; the step kernel stays dual-ABI). Test counts
   unchanged: 142 x86 / 151 aarch64+macOS.
CI workflow now mirrors build/test failures into check-run
`::error::` annotations (log/blob hosts unreachable from the
sandbox) and rebuilds the macOS unit-test leg with
`-fsanitize=address,undefined` on runtime failure — the ASan report
is what pinned #3 in one cycle. Notion "AI Notizen" page updated
with the close-out (CI green confirmed). PR #51 awaits the user's
merge (R17).

## Previous task (2026-10-25 implementation round + Android keyboard/CI —
merged as PR #47/#48/#49/#50)

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
- [x] **M6 Docs + baseline.** Roadmap checkboxes ONLY for the fully
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

### M6 results (2026-10-25, logged per R2/R3)

**Done:** README Roadmap now has exact function references on every checked
box and no fake checkboxes (min-p, AVX2, safetensors, KV-cache are `[x]`;
"full transformer forward pass" and the `.hydra2` MoE container stay `[ ]` —
the primitives exist and are tested but no forward pass is wired). The
NEON/AVX2 honesty note no longer says x86 is purely scalar.
`docs/ARCHITECTURE.md` gains the Components rows (`src/hydra_simd.h`, the
two converters, the sampler + primitives), the active-AVX2 SIMD table row
with the int32-exactness bound, and a new M4 primitives + KV-cache section.
`docs/GGUF-IMPORT.md` gains §10 (safetensors parsing, both-ends offset
validation, the F32/F16/BF16 set, the v2 output, the three-way verification).
`docs/FORMAT.md` no longer claims "no shipped writer for version 2" —
`tools/safetensors_to_hydra.py` is that writer now. `AGENTS.md`'s stale
hard-coded "38 checks" is replaced with "N checks, 0 failed (N parsed, never
assumed)". `tools/test_count_baseline.txt` is unchanged (x86 142 / arm 140):
M5 added no C tests, so the measured C count is unchanged.

**Measured:** docs_claim_check all banned claims absent; zero CJK in the six
audited docs; test count 142/0 x86 matches baseline.

### Android keyboard fix + full .md audit (2026-10-25, follow-up round)

**User request (screenshot of the broken chat UI):** the app UI breaks
while typing — the available space must be checked intelligently so the
keyboard pushes the content down; check the normal README plus all other
.md files; create a LOCAL scan script that is gitignored; "scanne alles,
dann PR".

**Diagnosis (verified in code, not guessed):**
- `Theme.Material.Light.DarkActionBar` (platform theme, manifest) draws a
  second "Hydra-Stone" bar above the Compose `TopAppBar` — doubled title,
  wasted vertical space.
- No `enableEdgeToEdge()` anywhere; classic `adjustResize`. When the
  keyboard opens the window shrinks, but the fixed header rows (banner,
  model row, engine status, selector, auto-run) plus the `LazyColumn`
  with `heightIn(min = 120.dp)` plus the input row overflow — the
  composer gets pushed off/under the keyboard. The existing
  `imePadding()` on `InputRow` is a no-op in this configuration (with
  `adjustResize` + windowed content the IME inset reports 0), and its
  comment claimed otherwise.

**Fix:**
- New `Theme.HydraStone` (values + values-night, `NoActionBar`,
  `windowLightStatusBar` per mode) — manifest updated.
- `MainActivity.onCreate()`: API 30+ → `enableEdgeToEdge()` +
  `SOFT_INPUT_ADJUST_NOTHING` (IME arrives as a real inset; the
  `imePadding()` on the composer is now the mechanism, and the comment
  says so); below 30 → classic `adjustResize` (no IME insets on the
  platform, the adaptive layout absorbs the resize).
- `AdaptiveHeaderSplit` (custom `Layout` in `ui/HydraApp.kt`): the header
  block gets its natural height up to 45% of the space, scrolls
  internally beyond that, the conversation gets the remainder. A plain
  `weight()` cannot do this — a weighted child never returns its unused
  share (the composer would float above the bottom edge). `MessageList`
  loses the fixed 120dp floor (it overflowed the window exactly when the
  window was smallest) and the `ColumnScope` receiver / `weight(1f)`
  that came with it.
- `tools/android_autostart_check.sh`: the old "guaranteed minimum
  height" grep (scoped on `ColumnScope.MessageList`) is replaced by two
  checks scoped on `AdaptiveHeaderSplit` (headerFraction cap; remainder
  hand-over).
- CI caught (run after PR open): `tools/android_autostart_negative_test.sh`
  still removed the OLD behaviour (the 120dp floor) and looked for the
  OLD check name — so its scenario was a no-op. Fixed properly: the
  gate's cap check now greps the cap expression (`maxHeight *
  headerFraction`), not the parameter alone (a parameter nothing
  multiplies is a cap in name only), and the negative test's scenario
  is replaced by two — one per new check — each removing the real
  wiring. Local proof: gate 32/0 on the real tree, negative test
  15 proved / 0 could not fail (was 13/1 in CI).

**Full .md audit (21 tracked files, via the new local scanner):**
- 1 real doc bug found and fixed: `docs/ARCHITECTURE.md` SIMD table
  named `hydra_neon_accumulate_chunk()` — the kernel is
  `hydra_neon_accumulate_agg()` (src/hydra_neon.h); name + wording
  corrected.
- 1 real doc bug found and fixed: CHANGELOG.md M3 entry sat above M6,
  violating its own "Newest entries first" header — reordered.
- Everything else verified clean: non-English text (none; Greek
  mathematical notation is allowed as English technical writing),
  hard-coded test counts in the live docs (none), relative links (all
  resolve), doc-vs-code `hydra_*` symbols (all resolve; `hydra_v2_reader`
  is the documented specification-only exception, `hydra_test` the AVD
  name), README roadmap `[x]` boxes (all cite verifiable code), quoted
  deterministic baseline (engine reproduces it right now), changelog
  order (newest-first).
- `FORMAT-V2.md` examined and CONFIRMED CORRECT: its "no v2 loader"
  statement is about the `.hydra2` sparse-MoE container (still
  spec-only), a different v2 than the 4-weights/byte packing that M5
  implemented — the disambiguation block in the file says exactly this.

**Local tooling:** `tools/local_scan.py` created (10 checks, stdlib,
parses everything at run time, missing binaries → skip not fail) and
gitignored via a committed `.gitignore` entry. It found both bugs above
on its first run (the scanner itself had three false-positive classes —
Greek letters as math notation, quoted historical counts, JSON baseline
spacing — fixed in the scanner, not by weakening a doc).

**Gate (all bash, per the standing 5-part matrix):** make clean/make 0
warnings; hydra-test 142/0 + large-model 7/0; gguf_test 59/0;
gguf_speed ok; string_format_check 35/0/4; server_test ok;
console_train_test pass; contrast_check 17 AA; jni_signature_check SKIP
(no javac in sandbox, env-only); android_header_check 22/0;
android_autostart_check 32/0; android_crash_check 25/0;
llama_pin_check 5/0 (7fe450e v0.5.0); docs_claim_check all absent;
test_count_check ok (142, x86 baseline); npm run lint 0; git status
clean apart from the intended changes.

**Not verifiable in this sandbox:** the Android app itself (no SDK, no
emulator) — CI runs the Gradle build + lint; the edge-to-edge/IME
behaviour needs a device check by the user after the merge.

**Next:** commit, push, open PR (R17: user merges).

**CI follow-up (first CI run, user pasted the logs):** three defects
the sandbox cannot reproduce — and the lesson is now standing: ALWAYS
check CI after pushing (`gh pr checks` + annotations via API), because
the Android SDK, the aarch64 toolchain, and CodeQL only exist there.
1. Kotlin compile errors: the custom `Layout` approach did not
   compile — `Constraints` lives in `androidx.compose.ui`, and the
   `Layout` import did not resolve in this pinned Compose set
   (foundation 1.7.x via BOM 2024.09.03, transitive via material3).
   Replaced with `BoxWithConstraints` (same package as the already-
   compiling `Box`, constraints read in the same measurement pass,
   cap via `maxHeight.value * fraction` + the imported `.dp` extension
   so no Dp operator import is needed). Gate + negative control
   updated to the new wiring (cap expression grep; body weight(1f)
   grep); local proof 32/0 + 15/15.
2. aarch64 `hydra-run-arm` link without `-lm` (M4 float primitives)
   in the ci.yml determinism step — fixed.
3. Built-in CodeQL: `fopen(path, "wb")` in `make_v1_model` (0666
   world-writable under permissive umask) — replaced with explicit
   `open(..., 0644)` + `fdopen`.

**CI follow-up, rounds 2-4 (same day) — CI fully green:** the job-log
hosts stay egress-blocked, so the two Android workflows now emit their
failure from the build step itself as a check-run annotation (Kotlin
`e:` lines + gradle tail in one `::error::`; the lint job annotates
each lint finding individually). Annotations are API-readable and need
no permissions — that annotation delivered the real error:
`Unresolved reference 'value'` at `constraints.maxHeight.value`.
Root cause: `BoxWithConstraintsScope.constraints` is the pixel-based
`ui.unit.Constraints` (`maxHeight` = Float, no `.value`); the Dp value
is the scope's own `maxHeight` property (verified in the
foundation-1.7.3-era sources). Cap now
`(maxHeight.value * headerFraction).dp`; the deprecated
`SOFT_INPUT_ADJUST_NOTHING` dropped (API 30+ edge-to-edge delivers the
IME as an inset under the default mode — and it was the "0 findings"
lint culprit). Negative control re-anchored to the new expression.
Final state: runs 40237612415 (CI) + 40237612492 (Lint & Security)
on `5445742` — every check pass. **Next:** user merges (R17).

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

**CI round 3 (b53d738):** Lint & Security red — but the new flow
worked: gradle step success (non-fatal), check step failure (gate as
the single failure point, as intended). The PR comment was still
missing: lint.yml carries `permissions: contents: read` at workflow
level → the issues API call got 403 and was swallowed by `|| true`
(the exact trap the fallback damper is supposed to catch now exposes
the function itself). The CI run on 199221c (CI workflow) was red
too — the code tree is verified consistent, so either transient
infrastructure or a real compile error; the b53d738 run settles it.

**Round 4 (now):** job-separated permissions `issues: write`
(overrides the workflow level, scoped to the two Android jobs) + a
`::warning::` marker on the comment-fallback path (a broken
mirroring then becomes visible in the annotations). errors.md:
permission-403 entry. The next failed run posts the report as a PR
comment → read the actual lint findings → definitive fix. Commit
05dbbfe, pushed. Side finding from the analysis: CI (ci.yml) is red
at the gradle build step on 199221c AND b53d738 (db85ccd was green)
— same code in both → the most suspicious candidate is the
HydraApp.kt change of round 2, but a compile error vs. an
infrastructure error was not distinguishable from the sandbox (logs
UI-only).

**Round 5 (now):** closes the last diagnostic gap: "missing report"
is a second error class (gradle dies before the lint analysis, e.g.
on Kotlin compile errors → no report file). The gradle steps in
**ci.yml and lint.yml** now log the full output to a file; the
check/publish step builds the payload from whichever class occurred
(lint report OR gradle log tail, 150–200 lines) and publishes it in
both cases (step summary + PR comment). The CI APK build job gets
the same treatment (capture + publish + job-separated issues:write).
YAML validated (3 files). From the next failed run on, every Android
error is readable via the API — regardless of the error class.
Commit 9608e4d, pushed. Side finding: the 9608e4d lint job ran only
46 seconds and died before the lint analysis (no report) — the
footprint of a compile error.

**Round 6 (now):** ROOT CAUSE: `androidx.compose.ui.platform.
LocalResources` does NOT exist in this Compose version (BOM
2024.09.03) — the round-2 repair referenced a library API that does
not exist (an unspoken assumption, not even used anywhere in the
repo; Theme.kt uses LocalContext). A single compile error explains
everything at once: CI build step red (199221c + b53d738, same code),
lint job "no report" (dies before the analysis), 46-second job.
Fix: `import androidx.compose.ui.platform.LocalContext` +
`val resources = LocalContext.current.resources` — the same
Resources class, the canonical Compose access, an API the repo
already compiles. Nothing else changed (plurals, call style,
workflow). The failure artifact in lint.yml additionally gets
/tmp/lint-report.md + gradle-lint.log. Docs: errors.md
(non-existent API + error fingerprint "CI red + lint without report
= compiler, not a lint finding"), CHANGELOG + session.md. Result:
**CI and Lint & Security green on a983414**; the owner **merged PR
#48** (20:04Z, merge commit afae8c8 on main). The release on main
then ran fresh and is green.

### Round 2026-10-25 (post-merge: doc audit from a screenshot)

The owner requested (via screenshot of the GitHub README state, main
afae8c8) a full check: the screenshot's claims + the final docs in
every folder. New session branch `arena/c19193e9-bestai` (from
afae8c8).

**Verified and confirmed (measured/counted, no change):** token
sequence `[471, 386, 385, 386, 385, 386, 385, 386]` (starter model,
start_token=42, 8 steps) reproduced with a real build + run; CI
neon-test (aarch64 cross-compile + qemu, parity run 42 8 --json) +
NEON-vs-scalar inside the same ARM binary (test_engine.c); emulator
evidence in android/README.md; make_model.py asserts A[0] != 0 /
B[0] != 0; zero runtime npm deps + ESLint dev-only; `make ui` →
:8787 (server.js PORT); FORMAT.md "Version 2" +
test_version_two_format; .hydra2 unimplemented (no hydra2 in
src/include), FORMAT-V2.md is the spec; docs_claim_check.sh green.

**Found and fixed:** (1) license contradiction — LICENSE is the full
GPL-3.0 text (GitHub header shows "GPL-3.0 license"), the README said
"MIT" in two places (license section + structure tree), package.json
`"license": "MIT"` → all three now GPL-3.0 (LICENSE itself untouched
— a legal decision of the owner; the llama.cpp MIT references in
AGENTS.md/ANDROID-AUDIT.md are correct and stay). (2) a roadmap
reference to a "*Roadmap status*" section in docs/ARCHITECTURE.md
that never existed (git log -S) → now points at the real section
"Known Structural Redundancy", where the superseded A[i]/B[i] design
lives (line 106). Docs: CHANGELOG, errors.md (license /
cross-reference fingerprint). PR opened (R17: the merge is with the
owner).

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
recorded in `.agents/docs/gpu-feasibility.md` so nobody repeats them.

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
- [x] `.agents/docs/gpu-feasibility.md`: measured vs blocked separated, conclusion
      "do not start v2 yet" with the reasoning.
- [x] UI: focus-visible rings, `prefers-reduced-motion`, empty state for
      the engine panel, 44px touch targets on small screens.
- [x] Verified: `make test` 61/0, ARM/NEON 67/0 under qemu, gcc+clang
      `-Werror`, cppcheck, ASan+UBSan 61/0, ESLint clean, `node --test`
      18/18, trainer test, JNI gate (2 native methods), gradle
      `assembleDebug assembleRelease lintDebug` = *No issues found*.
- [x] PR #51 opened (2026-10-25), **merged by the user at `d0c7679`
      (2026-10-26), tag `v1.0.26`** — CI on the merge commit 19/19 green
      (all jobs incl. `neon-test` under qemu-aarch64-static, `build-test`,
      `gpu-bench`, `jni-signatures`, `llama-pin`, `android-apk`, `android-lint`,
      semgrep, CodeQL c-cpp + js-ts, `release-config`). M1 complete.

## Round 2026-10-26 (post-M1: Compose crash, regression guard, CI caches, auto-close, memory in repo)

- [x] **User crash report (10:57, Android 15 / arm64 / API 35, build
      1.0.25/1.0.26): chat tab throws on every composition** — `measured
      with an infinite max height and Compose doesn't support this`
      (foundation 1.7.3, BOM 2024.09.03). Every crash frame mapped 1:1 to
      the foundation source (line map verified identical v1.7.0–v1.7.3):
      root cause = PR #50's `HydraApp.kt` — the ChatTab **header** (itself
      `verticalScroll`) is nested inside the list body's
      `Column { verticalScroll { LazyColumn } }` across a composable
      boundary ⇒ inner scrollable measures with infinite max height ⇒
      `CheckScrollableContainerConstraints` throws.
- [x] **Fix (`HydraApp.kt`):** header de-scrolled; `AdaptiveHeaderSplit`
      bounds the header in a `BoxWithConstraints` height cap and applies
      `verticalScroll` only when the cap binds — invariant (one vertical
      scrollable per column) documented in the KDoc. Verified by the guard
      against the pre-fix source (compile in CI — no SDK in the sandbox).
- [x] **Regression guard (crash protocol, AGENTS.md §7):**
      `tools/compose_scroll_guard.py` (two detectors: lexical strict-region
      nesting + lambda call-site × same-file definition — the boundary case
      a lexical-only scanner misses) with 4 fixtures
      (`tests/fixtures/compose/`: 2 bad [4 findings + the crash shape],
      2 good [0]); gate `tools/compose_scroll_check.sh` = CI gate **1g**
      (lint job). Verified 7/7: real 13 sources clean, pre-fix `d0c7679`
      detected with the true pair, fixture counts exact (negative control
      R25).
- [x] **CI speed — measured, then fixed.** The reported "QEMU 15 min" is
      not the CI reality (measured: neon-test 23 s); the real cost = two
      uncached Android jobs (APK 7 m 23 s, lint 4 m 38 s). Shipped:
      `setup-android` rewritten ($HOME roots, skip-if-pinned guard,
      `gradle` input) + `actions/cache` in both Android jobs with one
      shared SDK key and a deps key; cache miss = today's exact path
      (zero risk); red builds never poison the deps cache.
- [x] **Issue auto-close (2026 research, see RESEARCH.md §3):**
      `tools/issue_verify.py` (stdlib, PyYAML optional) +
      `.github/issue-verifications.yml` +
      `.github/workflows/issue-verify.yml` (push→main + dispatch,
      `issues: write`): runs the mapped gates, closes only when ALL pass,
      audit comment with run URL; `--dry-run` verified end-to-end (both
      gates green ⇒ #52 would close). Convention (AGENTS.md §8): normal
      issues close natively via `Closes #N` in the PR body.
- [x] **README roadmap audit (owner request):** all 7 roadmap claims
      verified against the code (forward pass unwired, min-P, safetensors
      stdlib-only, AVX2 + "no AVX-512", v2 packing + parity gates,
      `.hydra2` unimplemented, KV cache) — all true; one precision note
      added (NEON v2 unpack is AArch64-only).
- [x] **Memory into the repo (owner request):** `.agent_memory/RESEARCH.md`
      created as the permanent in-repo research record (5 sections, incl.
      crash line map, CI cache architecture, tool inventory); the
      gitignore rule that kept it local removed; AGENTS.md §5 says the
      path must be kept current. New AGENTS.md §6 (missing tools →
      install, with the measured sandbox channels), §7 (crash protocol),
      §8 (issue conventions).
- [x] **Sandbox tool inventory (owner request "schau dir alle an"):**
      gcc/g++/python3/node/npm present; Docker/QEMU/aarch64-gcc/clang/java
      absent and **not installable in this sandbox** (apt egress blocked;
      verified, RESEARCH.md §5) — Docker cannot be installed here, and no
      gate needs a container runtime; zig-via-pip is the proven ARM
      toolchain substitute.
- [x] Issues audit: the only open issue is #52 "Problems?" (2026-10-26) —
      the in-issue version of this round's request; mapped for auto-close.
      Code-scanning alerts API: 403 for the Actions token on both
      endpoints (documented in RESEARCH.md §4; findings available via
      CodeQL job logs / Security tab).
- [x] Local gate suite (C 142/0, JNI 86/0, large-model 7/0, count vs
      baseline, gguf 59/0, string-format + negative, node 37/0 + trainer
      + contrast 17, all 11 guardrail gates incl. the new compose guard)
      + docs (CHANGELOG, this file) + 3 commits + **PR #53 opened
      (R17, user merges)**.
- [x] CI on PR #53: first red = `lint.yml` run created with zero jobs
      and failing in the same second (2026-10-26 10:30Z) — root cause:
      duplicate `key:` line in the android-lint cache save (PyYAML
      silently accepts duplicate keys, the GitHub workflow compiler
      rejects them; the local parse check was too permissive for its
      consumer). Fixed + permanent gate `tools/workflow_yaml_check.sh`
      (CI gate 1h, negative-control fixture, both parser paths green) +
      errors.md entry. All other jobs green (APK 6 m 56 s incl. the
      Kotlin fix, cold cache).

## Round 2026-10-26 (post-#53: local lint stack, PR/main CI split, code-scanning alerts, Environment.md)

- [x] **Owner prompt executed (no CI deferral):** `/tmp/lintenv` venv via
      pip = Cppcheck 2.22.0, clang-tidy (LLVM 23.1.4), ShellCheck 0.11.0,
      yamllint 1.38.0, cpplint 2.0.2 (pre-built wheels, no apt).
      Full output captured (Environment.md, Arena section).
- [x] **Six real pre-merge defects found by the strict pass, all fixed:**
      goto crossing an initializer (C++ ill-formed), missing feature-test
      macros (strict -std=c99) in both src files, uncast malloc + `= {0}`
      (C++ parse) in main.c, missing final newlines (hydra_engine.c,
      main.c, lint.yml), unguarded cd (workflow_yaml_check.sh), dead
      variables (string_format_negative_test.sh). C suite re-run green
      (142/0, 86/0, 7/0, count vs baseline); g++/gcc -fanalyzer 100 %
      clean; cppcheck --enable=all: 0 error/warning; clang-tidy: no
      real-analyzer findings; shellcheck clean; yamllint: remaining =
      cosmetic brackets + the (now-removed) 272-char mapping line.
- [x] **CI tier split:** ci.yml heavy jobs (neon-test, build-test,
      gpu-bench, android-apk) main-only via `if: github.ref == ...`;
      PR/feature-branch = lint tier (lint 1a–1h, jni-signatures,
      llama-pin) + full lint.yml (sanitizers job keeps the unit suite
      under ASan+UBSan on PRs). release.yml already main/tag-only.
- [x] **Code-scanning alerts replace issue-verify (owner: nothing
      hardcoded, no issue checks):** issue_verify.py +
      issue-verifications.yml + issue-verify.yml removed; new
      code-scanning.yml on `code_scanning_alert` (created): event-payload
      driven, PR comment (marker de-dup) or job summary, native
      auto-resolution on the fixing analysis. Verified: YAML gate,
      shellcheck, jq parsing of a synthetic alert payload.
- [x] **Environment.md created (in-repo, root):** per-agent sections,
      contract (own section only, evidence for every block, tested paths
      first, no silent CI deferral); Arena AI section with measured
      egress allowlist, present/blocked tools (date + error +
      workaround), and the tested local check paths (lint venv, strict
      compiler pass, zig ARM compile, 5-part gate).
- [x] **AGENTS.md:** §3 gate 1b (mandatory local C/C++ static analysis)
      + "read Environment.md first"; §6 Environment.md contract; §8 new
      alert convention (scanning results, not issues, nothing
      hardcoded); §5 table rows.
- [x] **Linear checked (owner request):** workspace has 4 projects
      (BetterCI/VibeCI, Jules OS, Ai_trainings_hub, StealthMod) — **no
      Hydra-Stone/BestAI project and no Hydra issues** (title search
      empty); all open issues belong to the other repos. Nothing to fix
      there; no project created (not requested).
- [x] RESEARCH.md §7 (replacement decision) + §8 (PyPI wheel path) +
      index; CHANGELOG entry written.
- [x] Docs final check + local gate suite + commit + PR (R17): commit 445d3c8,
      PR #55 opened (owner merges per R17).
- [x] **PR #55 first CI run:** tier split proven live (neon-test,
      build-test, gpu-bench, android-apk all `skipping` on the PR; lint
      tier ran). One red: ci.yml gate 1a compiles 5 TUs with command-line
      feature macros -> in-file defines collided under -Werror. Fixed
      (guarded defines, both compile modes rc=0) + regression test
      tools/ci_syntax_mirror_test.sh (parses the exact CI command,
      negative control reproduced the CI error verbatim) + mirror step
      in ci.yml. See RESEARCH.md section 9.
- [x] **PR #55 second CI run:** gates 1a+1b (gcc+clang exact syntax)
      PASS; the new mirror step failed on CI - ubuntu's default awk is
      **mawk**, which rejects the gawk-tolerated regex literal (local/CI
      drift inside the drift-guard). Fix: POSIX-safe awk only (field
      comparison, gsub+length), verified incl. clang branch via
      clang->gcc wrapper; negative control re-proven against 580acce
      files (rc=1). RESEARCH.md section 9 updated.
- [x] **PR #55 third CI run: lint tier fully GREEN** (14 pass / 4 heavy
      jobs skipped by design). Third mirror fix: the -v pattern
      `Syntax check \(gcc` itself carried a backslash escape (awk
      processes escapes in -v values; mawk strips it -> unterminated
      regex). Now matched via index() - the script has zero regex
      escapes / POSIX classes / match() left. RESEARCH.md section 9
      records the full three-step resolution. Remaining after merge:
      heavy jobs on the main push (R17 merge by owner).

## Round 2026-10-26 (round 3: macOS release red, PR-tier portability scan, size pin, audit)

- [x] **release.yml `build-macos` red nach #55-Merge diagnosed + fixed:**
      in-file `_POSIX_C_SOURCE` enables on glibc but *restricts* on
      Darwin (hides O_NOFOLLOW + MADV_SEQUENTIAL — neither is POSIX).
      Fix: `#if !defined(__APPLE__) && !defined(__MACH__)` around the
      defines in both src files. All modes re-verified (strict C99,
      exact CI 1a, C++ parse, 142/0+86/0+7/0, zig aarch64 rc=0).
- [x] **macos-strict-scan job (PR tier, ci.yml):** exact release-macos
      command `-fsyntax-only` (no artifact) + new
      tools/apple_portability_check.sh (R25 self-test, preprocessor
      asserts + broken-pattern fixture). Measured: -D__APPLE__ on
      glibc is NOT a Darwin simulation (recorded, honest split).
- [x] **sizeof(HydraEngine) = 664** (README said 656 = pre-rng_state
      size, stale): README fixed ×2, ARCHITECTURE.md got the full
      per-field derivation, hydra_model.h pins 664 compile-time
      (C99-portable typedef assert, verified x86-64 + zig aarch64).
- [x] **User's exact local commands executed + outputs captured:**
      pip wheels (venv incl. new flawfinder 2.0.20), gcc strict
      -Werror -fanalyzer rc=0, cppcheck --enable=all src/ include/
      (0 err/wrn, 10 style = test-only APIs, 0 real), clang-tidy
      (+concurrency-*: 0 findings; 155 total, all classified),
      flawfinder --minlevel=4 "No hits found", make test-run rc=0,
      gguf_test 59/0, server_test 37/37, test_count_check (needs
      <binary> <arch> args by design — run as-is: usage error,
      recorded; with args: 142 vs baseline ok) + jni_signature
      (SKIP javac, said out loud; CI covers).
- [x] **Audit `.agent_memory/AUDIT.md` (5 sections, file:line
      citations):** zero-heap verified (main.c:479 ≤512 B CLI malloc
      noted), 664 B measured, SIMD parity tests named, server.js
      hardening verified; math: 4096 layers ⇒ 128 integers (≈208 B)
      effective capacity, column-0 decode (hydra_engine_step line
      568) + 1D sampler logit, GGUF converter keeps ≤0.0075 % of 7B;
      M4 primitives unit-tested/NOT wired (38 test refs, 0 prod refs);
      .hydra2 spec-only (honest doc, naming-collision risk noted);
      hazards ranked (mmap TOCTOU SIGBUS = accepted design cost);
      P0-P3 plan (honesty layer → cross-dim mix + full-state decode →
      wire M4/attention per FORMAT-V2 spec → hardening).
- [x] Environment.md: venv line + flawfinder 2.0.20, zig ABSOLUTE
      -I quirk recorded (relative -Iinclude fails under zig cc).
- [x] RESEARCH.md §10 (Darwin inversion root cause + lesson),
      CHANGELOG round-3 entry, PR #55 body rewritten (facts only,
      per owner rule: no conversation/connector content in PRs).
- [x] Commit + push + PR (owner merges per R17): commit 3a57580,
      PR #56 opened; macos-strict-scan job runs on the PR for the
      first time. Note: this session's branch is arena/be94e6b7-bestai
      (the earlier arena/91276612-bestai remote branch was deleted by
      GitHub when PR #55 merged).
- [x] **PR #56 first CI run:** everything green EXCEPT the new
      macos-strict-scan - and inside it: step 3 (exact release-macos
      compile, syntax-only) PASSED = Darwin fix proven on real
      headers; step 4 failed because apple_portability_check.sh was
      host-centric (plain-compile assertion written for glibc is false
      on Apple). Reworked: host-aware + control-based (control TU with
      same includes sets the baseline; glibc marker defines measured
      and handled); both branches proven by forced simulation.
      RESEARCH.md section 10 extended.
- [x] **PR #56 second CI red (android-lint NDK step): fixed.** The
      universal 664-byte pin broke the armeabi-v7a (32-bit, ILP32)
      NDK compile - reproduced locally with zig arm-linux-gnueabihf.
      Fix: LP64-conditional pin (the documented 664 B is a 64-bit
      statement). All ABIs verified (x86-64, aarch64, arm-32-bit,
      C++ parse, exact CI 1a) + suites green. RESEARCH.md section 10
      extended. (The PR tier catching both reds before merge is the
      intended behaviour.)
- [x] **PR #56 final: 15/15 GREEN @ 95c6721** (owner merges per R17;
      base for round 4). Post-merge: verify the main release build
      (build-macos) is green.

## Round 2026-10-26 (round 4: true per-layer execution — format v3, full-dim decode, converters, 672 pin)

Owner directive (round 4): the "1D deterministic token automaton by
construction" conclusion was REJECTED as lazy documentation — the
per-layer non-linear forward pass is mandatory real engineering:
"NO STUBS, NO FAKES, NO MOCKS, ZERO HEAP", web-verify BitNet b1.58 +
bitnet.cpp I2_S, deterministic C tests (dim d>0 changes the stream;
multi-layer NOT reducible to load-time sums), local gates with real
outputs, "document everything".

- [x] **Web research verified (2026-10-26):** b1.58 W1.58A8 —
      arXiv 2402.17764 + 2504.12285 + HF bitnet-b1.58-2B-4T:
      W̃=RoundClip(W/γ,±1), γ=absmean; A8 = per-token absmax int8 in
      [−127,127]; kernel packs 4 ternary/byte, unpacks in SRAM, GEMV =
      add/sub. bitnet.cpp I2_S (arXiv 2410.16144 Table 1, read
      directly): codes 00=−1, 01=0, 10=+1, 11 unused, 128-element
      block-interleaved layout (microsoft/BitNet #412). CONFLICT
      RESOLVED: the repo's table (00=0, 01=+1, 10=−1, 11=0) is a
      different but equally valid permutation — repo KEEPS its table
      for v2/v3 file compatibility; no layout-identity claim anywhere
      in the docs. Recorded in .agents/docs/RESEARCH.md (new).
- [x] **Format version 3 (docs/FORMAT.md canonical):** same 24-byte
      header, version=3, dim%4==0. Per layer: Wg (dim²/4 B) + Wu
      (dim²/4 B) + w1 (dim/4 B) + scale u16 LE (absmean·256, min 1) —
      the scale sits AFTER the w1 plane. Final W_out (dim²/4 B).
      layer_bytes = dim²/2 + dim/4 + 2. Errors: -10 (body size),
      -14 (cross-version step), -15 (scale 0), -16 (non-finite),
      -17 (kv append).
- [x] **Engine: `hydra_engine_step_tf()`** — per token: per layer
      xeff[i]=clamp127(state[i]+w1·token) → ternary mat-vecs
      gate=Wg·xefx, up=Wu·xefx (int32 exact, `tf_row_dot`) → /scale →
      SwiGLU → RMSNorm → RoPE(step_count) → state=clamp127(n·32); once
      per token KV sink-0 residual + append(k=v=state); then
      y=W_out·state (UNscaled int32), greedy argmax over ALL dim
      (token = i mod vocab); `hydra_sample_token()` v3 mode feeds y
      into the existing repetition/softmax/min-p/seeded sampler.
      Zero heap: stack arrays only, weights read from the mmap.
      step/step_fast refuse v3 → -14 (and vice versa). Loader: v3
      branch (no aggregation build; "v3-Modell geladen" log).
      Struct: +step_count (u32) → **sizeof(HydraEngine) 664→672
      (LP64 pin in include/hydra_model.h)**.
- [x] **REAL engine bug found + fixed (scale offset):** step_tf read
      the u16 scale at w1p[0..1] (the w1 bytes!) instead of
      w1p+dim/4 → sf=1/256. POS test passed by accident (SiLU
      saturation + RMSNorm scale invariance); NEG: g=−256 →
      sigmoid float-underflow → dead state. Diagnosed via instrumented
      /tmp copy (gcc -Isrc), fixed + comment. Hand-built 2-bit
      decode tests (POS +64/token, NEG −64/token 1) guard it.
- [x] **main.c:** v3 routes to step_tf in bench + generation (KV
      loop, caller-owned storage), --fast reported N/A on v3.
- [x] **tests/test_engine.c: +32 arch-independent checks (142→174):**
      v3 writer helpers (byte-precise, hand-derived), flip contrast
      (dim d>0 changes v3 stream; identical v1 twin invariant),
      4-layer non-reducibility vs load-time plane sums, KV sink/
      append semantics, 2-bit hand decode POS+NEG, validation
      (scale 0 → -15, short body → -10, dim%4 → -8, healthy control),
      cross-version refusals (±14), prefill == manual step_tf, seeded
      sampling reproducible. Test-writer bugs found while building
      these (v3_fill_block OR-smash: missing +j/4; OR-only pack
      couldn't clear bits) — both fixed, mask-then-set.
- [x] **tools/test_count_baseline.txt:** x86 142→174, arm 151→183
      (+32, arch-independent; ARM re-measure on the CI leg).
- [x] **Converters v3 (both):** --v3 + --v3-layers N. Gate/up =
      ternarised top-left dim×dim slices of cycled decodable tensors
      (cands[(2l)%n], cands[(2l+1)%n]); w1 = cycling embedding rows;
      W_out = first tensor; per-layer scale = max(absmean_g,
      absmean_u)·256 clamped [1,65535]; **stdlib-only write-time
      roundtrip check** (re-read, re-decode first scale + full W_out,
      abort on drift); report names every source tensor/γ/density +
      honest "dim×dim slice" note. decode_rows returns a FLAT list
      (rows concatenated) — caught when the w1 row read indexed a
      float.
- [x] **tools/gguf_test.py: +14 gates (59→73):** v3 GGUF + v3
      safetensors conversions, byte-level layout (header, body len,
      non-zero scales, non-zero W_out), two-run engine determinism,
      --dim 10 / --v3-layers 0 refused (ap.error raises SystemExit —
      caught explicitly, uncaught it kills the test process).
- [x] **Docs:** FORMAT.md (v3 section: corrected forward-pass
      pseudocode — token enters xeff BEFORE the mat-vecs, KV is
      once-per-token after the loop; layout table; validation rules
      incl. v3 errors; converter writers + roundtrip check),
      ARCHITECTURE.md (new "Version 3 forward pass" section —
      previously referenced but missing; M4 "not yet wired" → wired;
      Memory Model 672 per-field table; v1 "algebraically free"
      scoped to v1/v2; testing strategy +2 bullets), README
      (672 B ×2, v3 feature bullet, roadmap item checked, math
      section v3 note, project structure), CHANGELOG round-4 entry,
      .agents/docs/RESEARCH.md new (BitNet b1.58 + I2_S facts + repo
      decisions), this file.
- [x] **Local gates (round 4, real outputs):**
      `make test-run` → `73 checks, 0 failed` (gguf_test) +
      `=== 174 Tests, 0 failures ===` (hydra-test) +
      `86 checks, 0 failed` (jni-batch) + `7 checks, 0 failed`
      (large-model), rc=0. `gcc -fanalyzer` (strict C99 -Werror for
      engine+main; Makefile flags for the test TUs) → all 4 TUs rc=0.
      cppcheck --enable=all src/ include/ → 0 error/warning/
      performance/portability (12 style `staticFunction` = public M4
      API used by the test TU, same classification as round 3).
      flawfinder --minlevel=4 → `Hits@level [4] 0 [5] 0`.
      test_count_check → `ok test count 174 (arch 'x86')`.
      py_compile (both converters + gate) rc=0. clang-tidy 23.1.4
      (PyPI wheel, venv) on engine+main: new v3 code 0 bugprone
      findings (fixed: unsigned shift literals `2u/4u/6u`, uint32_t
      scale decode, (size_t) offset casts — the checker's caret is
      misleading, the SIGNED operand was the int literal); 26
      pre-existing signed-bitwise (v2 unpack/RNG, benign, classified)
      + 7 swappable-parameters remain, all in legacy code;
      concurrency-*/security-analyzer: 0.
- [x] **RepoInspect PDF (user-attached): INVALID for this repo.**
      It names Pydantic/LangChain/Poetry (not used), "1,482 files /
      918 contributors" (repo has 122 tracked files), and a CRITICAL
      at `BestAI/chains/llm_math/prompt.py:40` — no `chains/` dir,
      no `prompt.py`, no such dependency anywhere. Generic/placeholder
      report (most panels "Calculating..."); findings ignored, told to
      the user.
- [x] **Commit + push + PR (R17: owner merges)** — branch
      `arena/f2400a75-bestai` (session branch; main @ d6aeb0d already
      contains the round-3 merge of PR #56).

## Round 2026-10-26 (round 5: UI/UX bugs + expanded settings — user request)

User request: "find a way to make everything possible, expand settings with
various functions (model-related, thinking etc.), make the UI better, find
UI/UX bugs". Screenshots: Android app, landscape with keyboard open shows
the composer at the TOP of the screen with zero visible chat. Attached
"RepoInspect" PDF is INVALID for this repo (names Pydantic/LangChain/Poetry,
`chains/llm_math/prompt.py`, "1,482 files / 918 contributors"; the repo has
122 tracked files, C engine + stdlib Python, no such files) - ignored and
told to the user.

### Plan (verified in code before writing it)

**A. UI/UX bug fixes (all with code evidence):**
- [x] A1: "state 17 B" in the model row is the WRONG metric.
      llama_jni.cpp:426 reports `llama_state_get_size(ctx)` - per the pinned
      llama v0.5.0 header (line 818-822) that is the SERIALIZED state size at
      the current position ("Only use when saving the state"), which is ~17 B
      at n_past=0. The comment in the JNI ("logits plus KV cache ... a
      measurement of the library's own allocation") is false. Fix: add
      `kv_bytes` (2 * n_layer * n_ctx * n_embd * 2B - type_k/type_v default
      GGML_TYPE_F16, confirmed in llama-context.cpp:3721-3722) and
      `logits_bytes` (n_vocab * n_embd * 4B) to the load report; header row
      shows "kv" instead of "state"; fix the comment.
- [x] A2: the chat never auto-scrolls. MessageList (HydraApp.kt) is a
      LazyColumn with no position effect - new/streaming tokens are invisible
      until the user scrolls in a long conversation. Fix:
      rememberLazyListState + LaunchedEffect keyed on (last message id, last
      message text length); follow ONLY while the user is at the bottom (no
      yanking back over history they read).
- [x] A3: keyboard stays open after Send, covering the streaming answer.
      Fix: hide the IME in sendMessage (FocusManager.clearFocus +
      LocalSoftwareKeyboardController / WindowInsetsController).
- [x] A4: the screenshot-2 layout is a REAL defect, not a transition artifact:
      `imePadding()` sits on the InputRow, so on API 30+ (edge-to-edge,
      ADJUST_NOTHING) the keyboard inset inflates ONLY the row and eats the
      weighted space of AdaptiveHeaderSplit - in landscape (or any short
      window) TopAppBar + nav bar + keyboard consume the whole window, the
      split collapses to 0 and the composer renders directly under the
      TopAppBar with no chat visible (exactly the screenshot). Fix: apply the
      IME inset to the ChatTab root column so the WHOLE column (split +
      telemetry + row) sits above the keyboard; the split keeps its visible
      share.
- [x] A5: RunTelemetryRow jumps in height on the first run (returns Unit
      before) - reserve a min height.

**B. Expanded settings (the explicit ask):**
- [x] B1: real SettingsTab (replaces the "coming soon" block): Thinking /
      chain-of-thought switch (model reasoning: prompts the model to show
      step-by-step reasoning), system-prompt field, temperature 0..2 slider,
      top-k 1..100, min-p 0..0.5, repetition penalty 1..2, max tokens
      32..1024, context size 512..8192 (llama; needs reload, said so),
      reset-to-defaults. Benchmark / engine info / new conversation / log
      stay.
- [x] B2: wire-through: EngineInterface.generate gets a GenerationParams
      (temperature/topK/minP/repeatPenalty/maxTokens); llama_jni.cpp
      build_sampler_chain becomes parameterized (top_k, min_p +
      llama_sampler_init_rep_penalty - API confirmed in the pinned header),
      chain rebuilt on key change (key = the tuple, not just temp);
      LlamaBridge/LlamaEngine signatures follow; HydraEngine ignores the
      sampling params (deterministic engine - documented on the interface).
      System prompt + thinking prompt prepend in MainActivity, llama only
      (EngineCapabilities.understandsTextPrompt). Context size -> n_ctx of
      the llama load.
- [x] B3: persistence: SharedPreferences for all settings + selected engine
      + auto-run (survives process death, not just rotation).
- [x] B4: real Models tab (replaces placeholder): current model facts
      (name, size, format, engine detail), import button, and
      "Remove imported model" (back to the bundled starter) - a delete the
      app currently cannot do at all.

**C. Verification (honest split):**
- [x] C++: g++ -fsyntax-only of llama_jni.cpp against the PINNED llama v0.5.0
      submodule headers (now initialized) + a minimal jni.h stub (no NDK in
      this sandbox; stub limits the check to syntax/semantics of the llama
      calls, stated as such).
- [x] Kotlin: careful manual review - no kotlinc in the sandbox; the Android
      CI tier (android-lint + APK) builds it, same as every previous round.
- [x] strings.xml: XML well-formed check + every R.string referenced in the
      new code exists (grep cross-check).
- [x] C engine untouched: `make test-run` re-run before the commit.

### Results (all executed, real outputs)

**A. Bug fixes (shipped):**
- [x] A1: load report now carries `kv_bytes` (2 * n_layer * n_ctx * n_embd
      * 2 B, F16 KV default at the pin - verified in llama-context.cpp) and
      `logits_bytes` (n_vocab * n_embd * 4 B) on top of `state_bytes`, which
      is kept but documented for what it is (serialized-state size at
      n_past=0, a few bytes of metadata - NOT RAM). The false JNI comment
      ("logits plus KV cache") is replaced with the pinned-header citation.
      Header row shows "kv" (detailFor), not "state".
- [x] A2: MessageList now holds a rememberLazyListState; a LaunchedEffect
      keyed on (last message id, last text length, lastIsUser) scrolls to the
      bottom while the user is AT the bottom, and ALWAYS for the user's own
      message. History you scrolled up to is never yanked away.
- [x] A3: sendMessage hides the IME + clears focus (InputMethodManager,
      Activity-level; no Compose plumbing needed).
- [x] A4: .imePadding() + .navigationBarsPadding() moved from the InputRow to
      the ChatTab root column. The weighted split now absorbs the keyboard
      height instead of the row inflating and eating the split's space -
      the screenshot-2 collapse (composer under the TopAppBar, zero chat)
      cannot happen on any window shape.
- [x] A5: RunTelemetryRow reserves height(28.dp) before the first run; no
      layout jump when telemetry first appears.

**B. Settings expansion (shipped):**
- [x] B1: real SettingsTab: Thinking switch (prompt-based: a reasoning
      instruction is prepended - no engine change, works for any instruct
      GGUF), system-prompt field, sliders for temperature 0..2, top-k
      0..100 (0=off), top-p 0.05..1 (1=off), min-p 0..0.5, max tokens
      32..1024, context 512..8192 (llama, "applies on reload"), reset-to-
      defaults. Sliders commit on release (local state, persist-on-commit -
      no SharedPreferences storm during a drag).
- [x] B2: EngineInterface.generate carries GenerationParams
      (temperature/topK/topP/minP); EngineInterface.load carries a
      contextWindow request (0 = engine default). llama_jni.cpp:
      build_sampler_chain(SamplerKey) - conditional top_k -> top_p -> min_p
      -> temp -> dist, rebuilt on tuple change, all four clamped+logged at
      the JNI boundary. LlamaBridge/LlamaEngine follow (context clamped
      256..8192 there, reported granted value back). HydraEngine accepts and
      documents that it ignores both (deterministic by construction).
      DEVIATION, stated: the pinned llama.cpp v0.5.0 chain has NO
      repetition-penalty sampler (verified in the pinned header - the
      inventory is greedy/dist/top_k/top_p/min_p/typical/temp/temp_ext/xtc/
      top_n_sigma/mirostat1/2), so no rep-penalty control exists in Settings
      rather than a slider that would change nothing.
- [x] B3: SharedPreferences persistence for every setting + selected engine +
      auto-run; restored in onCreate BEFORE prepareModel. BUG FOUND + FIXED
      on the way: prepareModel() never looked at imported.gguf on relaunch,
      so a persisted llama selection + imported GGUF silently ran the v1
      starter - it now loads the GGUF through llama.cpp as the authority
      (NOT the .hydra header rules - catching that reportModel would have
      rejected a valid GGUF was itself the fix).
- [x] B4: real Models tab: current model card (label + engine facts),
      import button, "Remove imported model" (deletes the imported file
      only - never the starter; falls back to the v1 engine + starter when
      the selected engine loses its model).

**C. Verification (real outputs, this turn):**
- C++: `g++ -std=c++17 -fsyntax-only -Wall -Wextra` of llama_jni.cpp against
      the PINNED v0.5.0 submodule headers + minimal jni.h/android/log.h
      stubs (no NDK in the sandbox; stub declared as such): SYNTAX OK, zero
      warnings. cppcheck (warning,performance,portability, std=c++17,
      pinned includes): clean.
- Kotlin: careful manual review of every changed hunk + brace-balance check
      on all 10 .kt files (no kotlinc in the sandbox; the Android CI tier
      builds, same as every previous round).
- strings.xml: XML parses (all values/*.xml); 106 strings; every R.string /
      R.plurals referenced in Kotlin exists (0 missing); no new string
      unreferenced; tools/string_format_check.py: 37 passed, 0 failed, 4
      unverified (all 4 pre-existing: err_version_human, log_model_imported,
      log_model_starter, ok_version - templates formatted in code).
- tools/compose_scroll_guard.py: "ok no nested scrollable containers"
      (A2's LazyColumn stays the body's only scrollable).
- JNI signatures cross-checked native<->Kotlin by type: load
      (jstring,jint), generate (jstring,jint,jfloat,jint,jfloat,jfloat,
      jobject), cancel(), unload() - all match.
- C engine untouched: make test-run -> `=== 174 Tests, 0 failures ===` +
      `86 checks, 0 failed` + `7 checks, 0 failed` + `73 checks, 0 failed`
      (identical to the round-4 baseline).
- Web console (reviewed, same mandate): node tools/server_test.js ->
      37/37 pass; node tools/console_train_test.js -> all assertions passed.
      Code audit found no new UI/UX bugs (abort + server-side cancel
      protocol, single-flight busy guard, no innerHTML, encoded paths all
      in place from the earlier audit rounds).

**Files:** android/app/src/main/cpp/llama_jni.cpp,
android/app/src/main/java/dev/hydrastone/{EngineInterface,HydraEngine,
LlamaBridge,LlamaEngine,MainActivity}.kt,
android/app/src/main/java/dev/hydrastone/ui/HydraApp.kt,
android/app/src/main/res/values/strings.xml.

---

## Rounds 6-11 (2026-10-26), moved verbatim on 2026-10-30 (round 13)

## Round 2026-10-26 (round 6: CI red fix, local Kotlin type-check, .agents/ reorg — owner request)

**Owner request (verbatim):** "CI ist rot! :(" — fix it; "Teste alles lokal
mit Python"; "Immer Internet benutzen" + install missing tools (pip/npm);
move all agent-specific docs into a new `.agents/` folder (README.md and
AGENTS.md stay at root); add Rules etc. there; improve the agent files;
create a user-rules file (`.agents/user.md`); read `.agents/` before any
code, update it at the end of the round; adjust Environment.md.

### Root causes of the red CI (main f79ff96, all four workflows)

1. **CI job 1c (cppcheck):** `uninitvar` ×2 — `tests/test_engine.c:2790-91`,
   the v3 report roundtrip test's `hx[16]`/`hy[16]` were passed to
   `hydra_engine_report()` before cppcheck could prove a per-iteration
   write. Fix: zero-init `= {0}` (codebase convention for
   incrementally-filled buffers).
2. **Lint & Security "Android lint" + Release "Android APK" + CI Android
   jobs:** `HydraApp.kt:1011` — round-5 `SettingSlider` took
   `valueRange: ClosedFloatingPointRange` and used `.last`/`.first`, which
   only exist on IntRange. Found via check-run annotations
   (`gh api .../check-runs/<jobId>/annotations`) because the job logs and
   artifacts are blob-blocked from the sandbox. Fix: the slider takes
   `start: Float, end: Float` + step; step count from the span; the 6 call
   sites converted.
3. **Code-scanning workflow: corrupted GitHub registration.** The workflow
   was registered 2026-10-26 with the file PATH as its name and a broken
   trigger set: it ran on EVERY push (runs #20-#22, event=push, 0 jobs,
   fails in <3s, no logs — the "zero-job" signature). The file itself is
   valid YAML (PyYAML + the repo's duplicate-key gate both clean). Fix:
   force a fresh registration by moving it to
   `.github/workflows/code-scanning-alert.yml` (same content, same
   `name: Code scanning alert handling`). After the merge, the old
   registration (path `code-scanning.yml`) is orphaned by the deletion and
   the new path registers from the known-good content.
4. **clang gate 1b: `-Wtypedef-redefinition` in hydra_model.h (C99).**
   `hydra_kv_cache_t` was typedef'd twice (forward at line 153 + repeated
   on the full definition at line 287) — legal in C11, a warning under
   clang `-std=c99` → `-Werror` red. GCC is silent (its diagnosis sits
   behind `-Wpedantic`), which is why every local gcc gate stayed green.
   Introduced in round 4, merged in PR #57; the "lint" job of ci.yml was
   red on main since the merge (timeline: 12:11 green, 13:37+ red).
   Fix: full definition is now a plain `struct hydra_kv_cache { … };`
   (the name stays bound by the forward declaration). Diagnosed by
   reproducing gate 1b locally with `zig cc -target x86_64-linux-gnu`
   (zig bundles a Clang frontend — new local capability, see
   Environment.md round-6 item).
5. **Kotlin `weight` — a false alarm, corrected after CI's K2 error.**
   The local stub check flagged `Modifier.weight(1f)` (×5) as
   unresolved; the first "fix" added
   `import androidx.compose.foundation.layout.weight`. CI's K2 compiler
   rejected the import: `Cannot access 'val RowColumnParentData?.weight:
   Float': it is internal in file`. Real API (Compose source,
   RowColumnImpl.kt): `weight` is a member extension of RowScope/
   ColumnScope — resolvable inside the scope lambdas, import forbidden.
   The import was removed (the round-5 code was right on this point);
   the stub now declares `weight` as scope members, faithfully rejecting
   the import form. Full story in `.agents/errors.md`.

### The local Kotlin type-check (the capability this round added)

No Android SDK is possible in the sandbox (Maven blocked), but the Kotlin
COMPILER is on the npm registry:
`npm install kotlin-compiler@2.4.21 javajre-linux-64@17.0.8` (kotlinc-jvm
2.4.21 on JRE 17.0.8, matching the CI Temurin 17 major). Built a faithful
stub classpath (`tools/kotlin_stub_check/stubs/`, 40 files: android.*/
androidx.*/compose/org.json + R generated from the real strings.xml) and a
runner: `bash tools/kotlin_stub_check/kotlin_stub_check.sh` → compiles all
10 app .kt files in ~20s. This caught root cause #2 locally, before CI. Verified: **0 errors** on the fixed tree
(only 3 warnings, 2 pre-existing redundant-`else`, 1 stub artifact).
Stub-fidelity rules learned (see `.agents/errors.md`): verify API
signatures on the web before stubbing (Compose `items` key lambda is
single-parameter `(item: T) -> Any` — the two-parameter one is
`itemsIndexed`; `weight` is a scope MEMBER, not a top-level
extension); one package per stub file; fun-interface for Java SAMs;
JVM signature clashes when stubbing var+fun pairs. A stub error means
app code OR stub is wrong — the real API is the tie-breaker (the
`weight` episode proved it in both directions).

### .agents/ reorg (owner directive)

Moved (git mv): rules.md, errors.md, session.md, Environment.md, CLAUDE.md
→ `.agents/`; docs/{ANDROID-AUDIT,ANDROID_SKILL,RESEARCH,
HYDRA2-RESEARCH,gpu-feasibility,pr-gpu-feasibility}.md → `.agents/docs/`;
`.agent_memory/` → `.agents/docs/{TECHNICAL-AUDIT,RESEARCH-LOG}.md`
(folder deleted). Root keeps README.md + AGENTS.md; `docs/` keeps the
product specs (FORMAT.md, FORMAT-V2.md, GGUF-IMPORT.md, ARCHITECTURE.md).
All path references updated repo-wide (44 files, incl. CI comments, tools,
code comments, AGENTS.md pointer table); `tools/docs_claim_check.sh` FILES
array extended with `.agents/docs/*.md` (gate still passes). New:
`.agents/user.md` (owner's standing directives, dated, 21 entries) and
`.agents/README.md` (index + maintenance rules). Historical CHANGELOG
entries keep their dated old paths on purpose.

### Verification (all local, real outputs)

- `make test-run`: 174/0 + 86/0 + 7/0 + 73/0 (unchanged).
- cppcheck (venv, --enable=all -Isrc -Itests): 0 issues.
- `tools/kotlin_stub_check/kotlin_stub_check.sh`: OK (0 errors).
- `bash tools/docs_claim_check.sh`: all banned claims absent.
- `bash tools/workflow_yaml_check.sh`: 6 files parsed, negative control ok.
- `python3 tools/string_format_check.py`: ok (39 checked).
- PyYAML full parse of all 5 workflows: clean (installed via pip — the
  stdlib-only fallback in workflow_yaml_check.sh cannot see syntax
  errors; it passed, so the rename risk is covered by both).
- **clang gate-1b replica (local, new):** `zig cc -target x86_64-linux-gnu
  -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra
  -Werror` on all 5 CI files → reproduced the #4 failure before the fix,
  5/5 clean after.
- **NEON path after the header change:** `zig cc -target aarch64-linux-gnu`
  full compile of both src files → rc 0.

**Files:** tests/test_engine.c, include/hydra_model.h, android/app/src/
main/java/dev/hydrastone/ui/HydraApp.kt (weight import removed — false
alarm), tools/kotlin_stub_check/ (new; stubs corrected for scope-member
weight), .agents/ (new folder: 5 moved files + 8 moved docs + user.md +
README.md), .github/workflows/code-scanning.yml →
code-scanning-alert.yml, 44 reference updates.

**Open (superseded by follow-up 2 below):** PR #59 open. After merge:
confirm no zero-job push runs and all four workflows green on main —
see follow-up 2 for what the forensics actually showed.

### Round-6 follow-up (after the first PR #59 CI round, 2026-10-26)

Two more reds surfaced in the first CI round of PR #59 and were fixed
the same day (commit after 303b5c8):

1. **Android lint job: gate crash + 2 real findings.** With the Kotlin
   compile finally fixed, the job reached the lint analysis for the
   first time in the repo's history. The annotation step crashed parsing
   the `.txt` report as XML (see errors.md) — hiding the findings.
   Real findings identified by resource audit: `models_empty_title` /
   `models_empty_body` unused (UnusedResources) — the models-tab empty
   state had never been implemented. Fix: implemented the empty state in
   `ModelsTab` (uses both strings, `model_none` placeholder removed from
   strings.xml, R stub regenerated: 105 strings + 6 plurals) and fixed
   the gate to parse the XML report with text/gradle-died fallbacks and
   best-effort annotation (R25: the gate must always report its
   findings).
2. **`Modifier.weight` import removed** (see root cause 5 in the list
   above — K2 rejects the import; real API = scope member, no import).
3. **clang gate 1b fixed** (hydra_model.h typedef redefinition, root
   cause 4 above) — verified in CI: the `lint` job of ci.yml turned
   GREEN on the PR after the fix.

Status after the second CI round: ci.yml `lint` PASS, all fast checks
PASS; Android lint was re-running with the fixed gate — its
annotations are the source of truth for any remaining findings.

### Round-6 follow-up 2 (third CI round, 2026-10-26 evening)

1. **The fixed gate worked — it published its first findings** (run
   40339750614): exactly two, both fixed in **3354a9f**:
   `AutoboxingStateCreation` at `HydraApp.kt:994` (`mutableStateOf`
   with a Float → `mutableFloatStateOf`, stub gained the matching
   top-level function) and `PluralsCandidate` at `strings.xml:143`
   (`log_context_applied` "%d tokens" → `<plurals>` with one/other;
   call site MainActivity.kt:658 → `R.plurals.…`; R stub
   regenerated: 104 strings + 7 plurals). Stub check 0 errors,
   string-format check ok.
2. **Code-scanning forensics finished:** the rename did NOT clear the
   old registration — BOTH (`code-scanning.yml` id 396960786,
   `code-scanning-alert.yml` id 397125007) are still `active` and both
   carry the file PATH as name (unique to these two; all other
   workflows show their real `name:`). The new name produced the same
   phantom zero-job runs, auto-retried by GitHub every ~13 min
   (13:59/14:12/14:26 — not our pushes). The loop stopped by itself
   after 14:26; nothing in the following 45+ min. `DELETE
   .../actions/workflows/{id}` is 404 (token scope) — if it wakes up,
   the owner must delete the stale workflow in the GitHub web UI.
   **Not blocking the PR** (push-event runs never appear in `gh pr
   checks`).
3. **Sandbox reset incident:** the local branch pointer came back at
   f79ff96 while all four commits lived only on the remote; worktree
   intact. Recovered with `git fetch` + mixed `git reset
   origin/arena/ed8432d1-bestai` (pointer + index only, worktree
   untouched → clean). New rule (errors.md): after every push-ending
   turn, verify `git log --oneline -1` == last pushed sha at the start
   of the next turn. `/tmp` also wiped — kotlinc reinstalled from the
   Environment.md recipe (~1 min).

**Open (superseded by follow-up 3 below):** PR #59 open, branch head
3354a9f.

### Round-6 follow-up 3 (fourth CI round + root-cause correction, 2026-10-26 evening)

1. **ROOT CAUSE CORRECTION — the "corrupted GitHub registration" theory
   was WRONG.** The phantom zero-job runs (0 jobs, same-second failure,
   run named by the file PATH) were **compile failures of any workflow
   file naming `on: code_scanning_alert`**: that event is a Webhook
   event, NOT a GitHub Actions workflow trigger event (not in the
   current docs trigger list — page runs `check_suite` → `create` with
   no such section — and actionlint rejects it: `unknown Webhook
   event`). Proof by causality: the phantom always named the file
   containing the bad event, and followed it across three commits
   (`code-scanning.yml` 13:37-13:53 → `code-scanning-alert.yml`
   13:59-15:11 → `lint.yml` 15:13, the latter after I moved the trigger
   in — where it again produced the zero-job failure AND the Lint&Sec
   PR run went missing). The `name` = path display and the 14:12/14:26
   "auto-retries" were red herrings (no `name:` to show on a
   compile-failed run; more dispatch attempts of the same broken file).
   PyYAML/the duplicate-key gate cannot see this: they validate YAML,
   not the Actions event schema.
2. **Action:** (a) deleted `.github/workflows/code-scanning-alert.yml`
   (1b0ace5); (b) reverted the `code_scanning_alert` trigger + alert
   job out of lint.yml; (c) NEW GATE 1i `tools/workflow_lint_check.sh`
   = actionlint (npm WASM build, `/tmp/al` self-recreating, one fresh
   node process per file — the WASM runtime crashes intermittently on
   multi-file processes) over every workflow file + in-gate negative
   control `tools/fixtures/workflow_lint_bad.yml` (the invalid trigger
   event itself; R25). Wired into ci.yml after gate 1h. Code-scanning
   REACTION stays native: CodeQL/Semgrep jobs report on every PR
   (annotations + check results) and alerts close themselves as fixed
   when the next default-branch analysis no longer reports them.
3. **Android lint round 3 (run 40346890465, 3354a9f):** both original
   findings cleared; the plurals conversion introduced ONE new finding
   — `MainActivity.kt:658 ResourceType: Expected resource of type
   string`: `Context.getString` does not accept `R.plurals.*`
   (`@StringRes` vs `@PluralsRes` is enforced by LINT, not kotlinc —
   the stub check cannot express resource annotations and stayed
   green). Fix: `getQuantityString(R.plurals.log_context_applied,
   new.contextSize, new.contextSize)`; stub `A_content.kt` gained
   `getQuantityString`; stub-check README gained a KNOWN-LIMITATIONS
   section (ResourceType / AutoboxingStateCreation / PluralsCandidate /
   UnusedResources / Compose transforms are CI-lint-only classes).

**Open (superseded by follow-up 4 below):** PR #59 open, fix commit
7c534b2 pending.

### Round-6 follow-up 4 (merge/revert incident + stage-2 API fix, 2026-10-26 evening — new session branch `arena/72470567-bestai`)

1. **Incident:** PR #59 was merged by the owner at 15:15:21 at head
   **1b0ace5** (the broken state — bad `code_scanning_alert` trigger in
   lint.yml; the fix commit 7c534b2 landed 3.5 min later, after the
   merge). Main went red immediately (zero-job compile-failure run of
   lint.yml). 16 s after the merge the owner opened **PR #60 (full
   revert of #59)** out of the red; the owner then CLOSED #60 and asked
   for "everything correct" to land (PR #62 was opened from
   `arena/ed8432d1-bestai` carrying exactly 7c534b2).
2. **PR #62 CI exposed stage 2 of the plurals fix:** all jobs green
   except Android lint — `MainActivity.kt:658:25 Unresolved reference
   'getQuantityString'`: **`Context` has no `getQuantityString`**, it is
   a `Resources` method (API 1, developer.android.com pattern
   `context.resources.getQuantityString(R.plurals.…, count, count)`).
   The stage-1 fix had added a fabricated `Context.getQuantityString`
   to the STUB (violating the "never guess stub members" rule) — the
   stub check passed, the real compiler failed. Final fix on this
   branch: `resources.getQuantityString(R.plurals.log_context_applied,
   new.contextSize, new.contextSize)` (the pattern the codebase
   already used at 395/536/796), fabricated Context stub member
   removed, errors.md entry expanded with the stage-2 lesson.
3. **State:** this session branch = main (0ad01dd) + 7c534b2 + the
   stage-2 fix. PR #62 (old branch) is red and should be CLOSED; the
   green vehicle is the new PR from this branch. Stale workflow
   registrations (396960786/397125007) remain listed but inert (no file
   at the path = no dispatch); PAT cannot delete them (404), owner can
   via the web UI.
## Follow-up after the merge (2026-10-26)

- [x] **Root cause of ALL zero-job phantom runs (2026-10-25 → 26):** `on: code_scanning_alert` is a Webhook event, NOT a workflow trigger event. GitHub's workflow compiler rejects a file that names it — the run is created with zero jobs, fails in the same second, and the run NAME is the file path. PyYAML (gate 1h) could never see this (valid YAML; the event name is a string).
- [x] **Fixed in gate 1i (new):** `actionlint` over every workflow file (npm, /tmp, ephemeral re-setup) + in-gate negative control. `bash tools/workflow_lint_check.sh` locally. ci.yml + lint.yml no longer name `code_scanning_alert`; auto-close policy unchanged (the alert workflow remains, its trigger is `check_suite`).
- [x] **Cherry-pick of 7c534b2 + the stage-2 fix + the root-cause docs shipped as PR #64** (arena/72470567-bestai): PR checks 15 pass / 4 skipped / 0 fail; Android lint green; both branch workflows success; zero phantom runs.
- [x] **PR #62 opened** (arena/ed8432d1-bestai, head 7c534b2): same content minus the stage-2 correction — its CI red = the stage-2 error itself, now fixed in #64. To be closed as superseded (owner merges).
- [x] errors.md + session.md + Environment.md updated; gate sweep green (workflow-yaml, actionlint + neg control, docs-claim, string-format, kotlin stubs, shellcheck); PR body facts-only.

**Status (2026-10-26 15:33):** PR #64 MERGED → main 19df869 (the PR tier is now fully green for the first time: build/test/neon + every lint gate). Remaining item: close PR #62 as superseded — done in round 7.

## Round 2026-10-26 (round 7: rule adaptation + .agents/ skills + all-.md audit)

**Session start (R33):** 2026-10-26 (branch `arena/e28fb029-bestai`, base main 19df869 = merged PR #64, all checks green). Read before touching code: AGENTS.md (full) → session.md (current round) → errors.md (top 3 entries) → rules.md (R1–R33, §6–§8) → user.md (5 owner rules + dated directives) → Environment.md (Arena section) → engine-ci-verify + ci-triage SKILL.md. Web research for the R33 root-cause question (2026 sources; only-enforcement-guarantees + context-dilution findings recorded under the R33 rule and in errors.md).

**Owner directives (2026-10-26, verbatim intent):**
1. „Passe deine Regeln an, du liest nie diese vorher“ — the read-`.agents/`-before-work rule kept being SKIPPED; adapt the rules so it reliably happens, and investigate why (2026 web research on agent context engineering).
2. „in .agents Ordner bitte mit skills füllen, lese dafür die, die auch wirklich in Claude Ordner sind“ — fill `.agents/` with skills; base them on the skills that really exist in `.claude/skills/`.
3. „Verbessere alle .MD Dateien suche alle .MD Dateien, in jedem Ordner“ — find and improve EVERY .md file in every folder.
4. „PR #62 was ist damit?“ — PR #62 is the red leftover of the #59-merge incident, superseded by the now-merged #64 → close it.

**Context:** PR #64 merged green at 15:33 (main 19df869 = stage-2 plurals fix + root-cause fix + gate 1i). PR #62 still OPEN (superseded). This round fixes the PROCESS (rules/skills/docs), not engine code.

**Diagnosis of the skipped reads (2026 sources, see R33):** (a) prose-only rules are followed probabilistically — "instructions raise the probability, only enforcement guarantees it" (prompt-architects, 2026); (b) context dilution — the root instruction file is loaded on EVERY request and a 2400-line session.md buries the protocol; (c) arXiv May-2026 study (1650 Claude Code sessions): compliance odds drop ~5.6% per additional function in the instruction set, file position/size had no effect → the fix is a verifiable marker + a gate, plus slimming.

**Plan:**
- [x] R33 added to rules.md (mandatory session-start read order + `**Session start (R33):**` marker in the current round section BEFORE the first code edit) + CI gate `tools/session_start_check.sh` (deterministic check of session.md + in-gate negative control per R25), wired into ci.yml as gate 1j.
- [x] `.agents/skills/` created as the CANONICAL skills home (2026 Agent Skills spec: name == dir, quoted description, what+when): the 3 skills moved out of `.claude/skills/` (git mv), `.claude/skills` is now a symlink to `../.agents/skills` (Claude discovery keeps working; R30: all agent docs live in `.agents/`).
- [x] 4 NEW skills (written from verified repo knowledge only, no invention): `ci-triage` (annotation-reading, zero-job signature, local gate replicas), `kotlin-stub-check` (setup/run + the stub-fidelity rules learned in rounds 5–6), `android-lint-gate` (annotation reading, lint-only classes ResourceType/PluralsCandidate/AutoboxingStateCreation/UnusedResources, correct APIs), `repo-docs-audit` (this round's audit made repeatable).
- [x] session.md slimmed: everything before round 6 moved verbatim to `.agents/docs/SESSION-ARCHIVE.md` (context-dilution fix; nothing deleted).
- [ ] All-.md audit (28 files listed in Workspace): stale paths, dead refs (deleted code-scanning workflow, old PR states), banned claims, R31 language.
- [ ] errors.md entry (skipped-reads → R33 + this round's findings); user.md entry for the 4 directives.
- [ ] Close PR #62 (superseded by merged #64; #62 CI red = the stage-2 error #64 fixed).
- [ ] Gates green (incl. new 1j), commit, push, PR (facts-only; owner merges).

**Progress:**
- [x] Session start: AGENTS.md + .agents/{rules,session,errors,user}.md read; 28 .md files listed; 2026 web research done (AGENTS.md budgets/dilution; enforcement-over-prose; Agent Skills spec) — findings recorded in R33 + errors.md + the ci-triage/repo-docs-audit skills.
- [x] R33 + gate 1j (tools/session_start_check.sh, wired into ci.yml after 1i; local run: ok, negative control proven).
- [x] `.agents/skills/`: 3 moved (session-workflow, engine-ci-verify, android-ndk-build) + 4 new (ci-triage, kotlin-stub-check, android-lint-gate, repo-docs-audit); `.claude/skills` = symlink.
- [x] session.md slimmed to current rounds (archive: .agents/docs/SESSION-ARCHIVE.md).
- [x] all-.md audit + fixes (28→33 files inventoried; 0 broken links; fixed: AGENTS.md §8 dead code-scanning.yml ref (rewritten to verified reality), FORMAT-V2 ↔ HYDRA2-RESEARCH cross-links, root README tree + CI-tier note + skill pointer, .agents/README index (skills table, R33, archive), android/README dual-engine reality (file tree, libs, run flow, tabs), CHANGELOG backfill (rounds 5/6/6-follow-up/7 were missing), RESEARCH-LOG §7 correction (event-triggered workflow could never run — deleted same day), session-workflow skill (R33 step + format template), android-ndk-build description (8 pitfalls), rules/README/AGENTS "32 rules" → 33)
- [x] errors/user docs (errors.md: R33 root-cause entry + the archive-split incident; user.md: directives 22–25)
- [x] PR #62 closed (superseded; factual comment records why: its CI red = the stage-2 API error fixed by merged PR #64)
- [x] commit + push + PR: **PR #65** (ec344a9). CI: **15 pass / 4 skipped / 0 fail** (run 40353536462 + 40353536695) — incl. the NEW gate 1j (session-start marker) green on CI, docs-claim green with the archive exclusion + negative control, Android lint green. Main-only tier jobs correctly skipped on the PR.

**Awaiting owner:** merge PR #65. Post-merge: main carries R33 + gate 1j; any session that skips the read protocol will turn the next push red.


## Round 2026-10-26 (round 8: full transformer attention (v4), .hydra2 sparse-MoE, Android sampler, CLAUDE.md fix — owner prompt)

**Session start (R33):** 2026-10-26 (branch `arena/fe34926d-bestai`, base main 3d13d5a = merged PR #65, all checks green). Read before touching code: AGENTS.md (full) → session.md (current round) → errors.md (top 3) → rules.md (R1–R33) → user.md (25 directives) → Environment.md (Arena section) → engine-ci-verify + ci-triage + repo-docs-audit SKILL.md → docs/FORMAT-V2.md (full), docs/FORMAT.md (v3 section), docs/ARCHITECTURE.md (full). PLUS the owner's directive for this round: read ALL .md files first — done (33 files, all folders; the 4 product specs, all 7 skills, all .agents files, both READMEs, CHANGELOG).

**Owner directives (2026-10-26 evening, verbatim intent):** "session.md kann bleiben" (do NOT optimize/rewrite session.md; it stays committed and readable by everyone — verify it is not git-ignored) + "habe ein prompt erstellen lassen" (the pasted Principal-Architect prompt is the work plan) + "lese alle dateien, alle .md dateien, als erstes, und dann bitte optimieren (außer session.md …)" + "weiter machen, suche auch im Web wenn nötig" + "Bitte intelligent" + "Finde Fehler und toten Code" + "lade über python weitere Linter und so herunter" + "KEINE Stubs oder Fake". No native todo tool exists in this environment — the plan below is the todo list (session.md precedent).

**Prompt audit, corrected against live main 3d13d5a (the prompt was written against a pre-round-4 state; R26 — verify before acting):**
- Claim "5 of 7 roadmap items checked; [ ] Full transformer forward pass, primitives NOT wired into an end-to-end layered forward pass" — **STALE.** Round 4 (merged via PR #57 → main) already shipped `hydra_engine_step_tf()`: per-layer ternary SwiGLU blocks + RMSNorm + RoPE + KV sink/append, format v3, tested (`test_tf_*`), roadmap item already `[x]`. What is genuinely missing for a FULL transformer: **attention** — no Q/K/V projections, no causal scaled dot-product attention.
- Claim ".hydra2 sparse-MoE: zero implementation" — **TRUE** (FORMAT-V2.md: "No part is implemented").
- Claim "hydra_jni.c does not expose hydra_sample_token / KV state to Android" — **TRUE** (HydraEngine.kt ignores sampling params; documented on the interface).
- Claim ".claude/skills symlink breaks on Windows; CLAUDE.md hidden in .agents/" — **TRUE** (web-verified 2026-10-26: Claude Code v2.1.277+ reads AGENTS.md only as a FALLBACK when no CLAUDE.md/.claude/CLAUDE.md/CLAUDE.local.md exists above the cwd; a committed symlink checks out as a one-line text file on `core.symlinks=false`; Claude's Edit/Write refuse symlinked paths; the import pattern `CLAUDE.md` containing `@AGENTS.md` is the cross-platform fix — sources recorded in RESEARCH-LOG §10).
- Invariant kept: the v1/v2 starter baseline `[471, 386, 385, 386, 385, 386, 385, 386]` (start_token=42) stays 100% intact — all engine work is ADDITIVE (new format versions / new functions), never a rewrite of the legacy path (user.md #11).

**Plan (milestones; order = dependency, each verified locally before the next — zero stubs, zero fakes):**
- [x] M-A: R33 session start — all 33 .md files read; round 8 section + marker; prompt audit corrected above.
- [x] M-B: session.md visibility — verified tracked (23 .agents files in git, `git check-ignore` clean); fix the STALE .gitignore comment (names the deleted `.agent_memory/` + top-level RESEARCH.md); add `.agent_memory/` as an ignore (local scratch only — repo memory lives in `.agents/` per owner decision).
- [x] M-C (prompt M4): real root `CLAUDE.md` (`@AGENTS.md` + `@.agents/CLAUDE.md`), docs updated (AGENTS.md §5 table, .agents/README.md, CHANGELOG); Windows symlink behaviour documented honestly.
- [x] M-D (owner: linters + bugs + dead code): pip-install additional linters (pylint, bandit, pyflakes for the Python tools; existing: cppcheck/clang-tidy/shellcheck/ESLint) → run everything → fix REAL findings; dead-code hunt (declared-but-never-called functions in C; unused imports/vars in Kotlin/JS/Python).
- [x] M-E (prompt M3): expose the C sampler (temperature / min-p / repetition penalty) through `hydra_jni.c` + `HydraBridge.kt` + `HydraEngine.kt`; default greedy calls 100% unchanged; kotlin stubs updated with the new `external fun`s; `kotlin_stub_check` + `compose_scroll_check` + `jni_signature_check` (CI javac) green.
- [x] M-F (prompt M1, corrected): FULL attention on top of v3 → **format version 4** (new version value, backward compatible: v1/v2/v3 files and the baseline stream untouched): per-layer Wq/Wk/Wv ternary planes + the existing Wg/Wu/w1/scale; `hydra_transformer_forward_step()` = pre-norm RMSNorm → ternary Q/K/V (NEON/AVX2/scalar dispatch) → RoPE → KV cache (sinks + window) → causal scaled dot-product attention (deterministic fixed-point) → residual → RMSNorm → SwiGLU FFN → residual → final RMSNorm → W_out logits; CLI + engine dispatch; tests incl. KV progression, RoPE position increment, min-p sampling, hand-computed case, NEON-vs-scalar same-build parity.
- [x] M-G (prompt M2): `.hydra2` sparse-MoE container per docs/FORMAT-V2.md — header parser + BOTH-ENDS validation (R14), top-k router (softmax over selected), expert streaming loader (pread, crc32 per read), fixture generator (make_model.py or gguf_to_hydra.py), tests (`test_hydra2_*` + security refusals + routing determinism).
- [x] M-H (prompt M5): roadmap checkboxes only after M-E/M-F/M-G are green (M6 invariant: 100% implemented + automated test); `tools/test_count_baseline.txt` updated in the same commit as the tests.
  - 2026-10-26: M-B..M-G deliverables each verified present in the tree before checking (M-B: .gitignore comment + `.agent_memory/` ignore; M-C: real root `CLAUDE.md`; M-D: linter runs + fixes from the previous round; M-E: `hydra_sample*` plumbing in `hydra_jni.c`; M-F: v4 loader/engine/tests + baseline stream re-verified; M-G: `src/hydra_v2_container.c` + 120-check `hydra2-test` (ASan/UBSan clean, gcc `-fanalyzer` clean on all three C files, fixture generator, CI wiring in `ci.yml` + `lint.yml`). Baseline file: x86 174->219, arm 183->228 with the dated entry (ARM re-measure on the CI leg). `docs/FORMAT-V2.md` updated: real implementation status, K/V wire format + 16-byte alignment recorded as `[CODE]` decisions.
- [x] M-I: .md optimization pass (all files EXCEPT session.md) — findings from the full read fixed in place; docs for the new versions/sampler/CLAUDE.md; CHANGELOG entry; Environment.md round-8 section (own section only).
  - 2026-10-26: all 33 .md files scanned for round-8 staleness. Fixed in place: `docs/FORMAT-V2.md` (implementation boundary, K/V wire format + 16-B alignment as dated `[CODE]` decisions), `docs/FORMAT.md` (new "Version 4" section: layout/forward pass/constraints/honest no-v4-writer status; the 4-GiB note no longer says "not yet implemented"), `docs/ARCHITECTURE.md` (new "Version 4 forward pass" section + components table), `README.md` (v4 feature line, Android sampler clause, the `.hydra2` roadmap item split into done-container [x] + open-inference [ ] with the spec-section evidence), `android/README.md` (the "ignores sampling params" claim is false since M-E: runInferenceSampled + fixed seed + temp-0 baseline), `CHANGELOG.md` (round-8 entry), `.agents/Environment.md` (round-8 section: pip linter installs + versions + full finding triage + recreate commands), `.agents/errors.md` (two real analyzer-found bugs + the regex-insertion process error). Files verified stale-free and left untouched: user.md, rules.md, the 7 skills, the .agents/docs/* research files (their "no v2 reader" statements are historical records of their own audit dates).
- [x] M-J: full gate sweep (AGENTS.md §3 order), commit, push, PR (facts only; owner merges).
  - 2026-10-26: gate sweep per AGENTS.md §3 order — every gate green or explicitly
    SKIP/blocked-out-loud (jni_signature: no javac, CI covers; ARM execution + NDK:
    no toolchain/SDK in sandbox, CI covers; mirror-test clang step: no clang driver,
    said out loud per R23). Full results in the commit message of 75b2649 and in the
    PR #67 description. Committed as 75b2649 (40 files, +5022/−212), pushed, PR #67
    opened (facts only). Merge: owner.

## Round 2026-10-26 (round 9: GGUF verification — spec conformance, the ggml_type bug, the pinned llama.cpp smoke, the behavior gate, the .hydra2 GGUF boundary, all-.md pass)
**Session start (R33):** 2026-10-26 (branch `arena/75337031-bestai`, base `0522d68` = merged PR #67). Read before the first edit: AGENTS.md, .agents/session.md (rounds 7-8), .agents/errors.md (top entries), .agents/rules.md (R2/R14/R23/R25/R26/R32/R33), .agents/user.md, .agents/Environment.md (round-8 section), docs/GGUF-IMPORT.md (full), docs/FORMAT-V2.md (header + boundary), README.md (Features + Android + tools tree), CHANGELOG.md (round-8 entry), tools/gguf_test.py + gguf_reader.py + gguf_to_hydra.py (current state), Makefile (gguf-test target) + .github/workflows/ci.yml (test-run), and - for the smoke test - the pinned llama.cpp source in /tmp/lc (llama-arch.cpp, llama-vocab.cpp, llama-graph.cpp, models/llama.cpp, llama-model.cpp, ggml.h enum) plus the official sources fetched fresh (ggml-org/ggml include/ggml.h + docs/gguf.md).


**Owner directive (verbatim intent):** "super, habe schon gemerged, dann prüfe nochmal, ob man dann GGUF dings modelle dort nutzen kann etc, also dass diese wirklich richtig schreiben, mit ihrer ganzen Intelligenz etc!!! mache tests, schaue im Web, und mache dann Doku etc!!!! alles an .md dateien aktualisieren!" — PR #67 merged by the owner as `0522d68` before this round started.

**Question answered (honestly):** the app is dual-engine. The **llama.cpp
engine** (submodule pinned at v0.5.0, `7fe450e19305b828c199d602c23a8337aaa1f03b`,
`LlamaEngine.kt` + `llama_jni.cpp`, `isExperimental = false` per
LlamaEngine.kt) is the main engine and runs GGUF with full transformer
inference — that is the "ganze Intelligenz" path. The `GGUF → .hydra`
converter is a documented 64-dim projection (seed model, not the source
network); its output now has a behavior gate that states exactly what it
can and cannot prove.

**Plan + status (all local-gated before commit):**
- [x] R9-A: web verification against official sources — ggml_type enum
      (ggml-org/ggml `include/ggml.h`) and the GGUF spec (ggml-org/ggml
      `docs/gguf.md`; the ggml-org/gguf repo does not exist).
- [x] R9-B: bug found by the enum audit — `tools/gguf_reader.py`
      quantised-name table wrong (31=TQ1_0/32=TQ2_0/34=MXFP4 vs official
      34=TQ1_0/35=TQ2_0/39=MXFP4, +40/41/42 missing, 31–33/36–38 reserved).
      Fixed; converter refusal now names tensor + layout
      (`token_embd.weight (TQ1_0)`); regression gate `run_type_names` in
      `tools/gguf_test.py` (6 current names + 6 reserved slots + message).
- [x] R9-C: spec conformance checked (header order, 13 KV types, tensor
      info, offsets relative to data section, alignment; version 2–3).
      LE-only = spec-conformant (spec: assume LE, not detectable);
      64-byte name cap not enforced (harmless) — both documented.
- [x] R9-D: behavior gate `run_behavior` (converted model: two identical
      `./hydra-run` runs + ≥2 distinct tokens, not the input repeated,
      for (0,24)/(3,40)/(11,16)); measured streams before choosing the
      threshold (token 0 → 3 distinct, tokens 3/11 → 2 distinct; ≥3 would
      have failed on the real 4-token fixture — 16-token scratch fixture
      gave 8–9 and does not count).
- [x] R9-E: pinned llama.cpp smoke — built the pin (codeload tarball,
      `cmake -B build -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF`,
      `--target llama-cli llama-server`); `tools/gguf_minimal_llama.py`
      (NEW, stdlib, fixed-seed LCG, 2 layers / dim 64 / heads 4 / ffn 192,
      99-token BPE charmap, empty merges, 21 tensors) → `llama-cli -m
      /tmp/smoke.gguf -p "a" -c 64 -n 4 -t 1 --temp 0` on the pin:
      **prompt 36 082.5 t/s, generation 5 640.6 t/s, generated text
      `1'aV+uE~`** (random by construction, byte-identical across
      runs, `llama-cli -p "a" -c 64 -n 8 -t 1 --temp 0`). Generator
      fixes from the run + the Qodo review (R9-I):
      `tokenizer.ggml.model`="gpt2" (not "BPE" — "unknown tokenizer"),
      `token_type` INT32 with the pinned enum (chars NORMAL=1,
      specials CONTROL=3 — the original file had them REVERSED, which
      is why the first smoke printed nothing),
      `tokenizer.ggml.add_bos_token` / `padding_token_id` (not the
      `llama.*` names — those are parsed but ignored by the pin),
      `output_norm.weight`, `llama.feed_forward_length`, LCG weights
      with real signs, ffn layout `{n_embd,n_ff}` / `{n_ff,n_embd}` in
      this build. F16 variant reaches a ggml type mismatch in the pin
      (f32 act × f16 norm in `ggml_mul`) → `--dtype f32` default
      (library + gate + docs aligned). `run_minimal_llama` keeps the
      generator alive in CI (dtype, token types, keys). llama-cli in
      this build is always-REPL: closed stdin → `> ` prompt
      busy-loop (sandbox artifact, documented).
- [x] R9-F: `.hydra2` boundary — `src/hydra_v2_container.c` refuses the
      GGUF magic BY NAME (bytes 0x47 0x47 0x55 0x46, LE u32
      0x46554747; points at LlamaEngine + converter +
      docs); +2 checks in `tests/test_hydra2_container.c` (suite 120→122;
      `tools/test_count_baseline.txt` M-G note updated in this commit).
- [x] R9-G: ALL .md updated — GGUF-IMPORT.md §11 + §4/§9, README
      (dual-engine, import bullet, tools tree, 122), FORMAT-V2.md
      (GGUF not a v2 container), CHANGELOG (round-9 entry + 122),
      errors.md (round-9 mistakes), Environment.md (round-9: /tmp reset,
      venv /tmp/lintenv2, llama.cpp build recipe), this section.
- [x] R9-H: gate sweep + commit + push + PR #68 (facts only; owner
      merges). Local results: gguf_test green incl. the three new
      gates; hydra2-test 122/0; flawfinder 0; cppcheck 0; make 0
      warnings; bandit medium+ 0 (20 Low = triaged asserts/subprocess,
      lint.yml comment); pyflakes clean; v1 baseline `471 386 385 386
      385 386 385 386` unchanged; llama.cpp build + smoke as above.
- [x] R9-I: Qodo code-review fixes on PR #68 (all 11 findings; every
      one verified against the sources before acting — all were
      valid): (1) **HIGH**: the R33 session-start marker (top of this
      section) was missing — `tools/session_start_check.sh` would
      have been RED in CI; (2) the two tokenizer keys renamed to the
      `tokenizer.ggml.*` namespace the pin actually reads
      (llama-arch.cpp L394/397 — the `llama.*` names are parsed but
      ignored); (3) README: llama.cpp availability per ABI (arm64
      selectable / x86_64 packaged but not selectable / armeabi-v7a
      absent — MainActivity.kt L84-94 + CMakeLists verified); (4)
      `run_behavior` now asserts the exact requested token count;
      (5) token types NORMAL(1)/CONTROL(3) — the original file had
      them reversed (UNKNOWN chars are dropped by the BPE
      detokenizer: the root cause of the first empty smoke output);
      (6) dtype defaults aligned on f32 (library, gate, docs);
      (7) §11.4 intro now describes the fixture as F32; (8) LCG
      produces real (-1,1) weights (was [0, 2·scale)); (9) GGUF
      magic written as bytes 0x47 0x47 0x55 0x46 / LE u32 0x46554747
      (the C message had the BE value 0x47475546) — C, FORMAT-V2,
      CHANGELOG; (10) generator docstring: stale numbers replaced by
      a pointer to §11.4, fact count corrected; (11) the §11.4
      recipe runs the generator from the repo root (it lived in a
      post-`cd` directory that does not contain tools/). Smoke
      re-run after the fixes: prompt 36 082.5 t/s, generation
      5 640.6 t/s, text `1'aV+uE~` (deterministic). NOT implemented
      (owner call, per the standing "CI speed up safely" directive):
      a CI job that builds the pin and runs the smoke automatically —
      Qodo's optional suggestion, 10–15 min per PR. The manual
      recipe is documented in §11.4; re-run it whenever the pin
      changes.

## Round 2026-10-26 (round 10: the documented smoke recipe bug, all-.md English pass, real-model verification, the opt-in real-model gate)

**Session start (R33):** 2026-10-26 (branch `arena/75337031-bestai`, base `634a4fb`). Read before the first edit: AGENTS.md, .agents/session.md (rounds 8-9 + R9-I addendum), .agents/errors.md (top entries), .agents/rules.md (R2/R14/R23/R25/R26/R32/R33), .agents/user.md (directive list), .agents/Environment.md (round-9 section), docs/GGUF-IMPORT.md (full, esp. 11.4 recipe), README.md (GGUF + Android bullets, tools tree), CHANGELOG.md (round 9), docs/FORMAT-V2.md, the GitHub issue the owner linked (the `timeout` recipe bug, GGUF-IMPORT.md 458-460), and — for the English pass — every *.md in the repo (systematic scan, 33 files) plus .github/workflows/{lint,release,ci}.yml and the Makefile; for the real-model work: tools/gguf_test.py (main/run_behavior/run_v3), tools/gguf_to_hydra.py (dim/vocab handling), tools/gguf_reader.py (is_decodable/decode_rows), android/app/src/main/java/dev/hydrastone/{LlamaBridge,LlamaEngine}.kt + android/app/src/main/cpp/llama_jni.cpp (the load path).

**Owner directive (verbatim intent):** "prüfe folgendes [the Qodo issue: the documented smoke command makes `timeout` treat `OMP_NUM_THREADS=1` as an executable], und prüfe CI und alles auf englisch dort übersetzen, dann alle .md dateien analysieren und bearbeiten, prüfe die tests, lade dir wirklich ein Modell herunter, was evtl so 300 MB ist und was schreiben kann, und finetune alles, ermögliche alles auf der app."

- [x] R10-A: recipe bug fixed + verified — the §11.4 line was
      `(sleep 10 | timeout 10 OMP_NUM_THREADS=1 $LLAMA/…/llama-cli …)`:
      `timeout` takes `OMP_NUM_THREADS=1` as the command to run, so the
      documented recipe NEVER launched llama-cli (reproduced:
      `timeout: failed to run command 'OMP_NUM_THREADS=1'`). Now
      `timeout 10 env OMP_NUM_THREADS=1 …`; the fixed recipe was
      executed verbatim from a clean cwd and printed the fixture
      output (`1'aV+uE~`, prompt 34 410.1 t/s, generation 6 323.4 t/s).
      errors.md entry added.
- [x] R10-B: all .md + CI in English. Systematic scan (word-boundary
      regex, umlauts + strict German function words; the first scan
      was a false-positive disaster — "commit" contains "mit").
      Translated: .github/workflows/lint.yml (6 blocks), release.yml
      (15 blocks), Makefile (1 line), .agents/docs/SESSION-ARCHIVE.md
      (rounds 3-6 + doc-audit section, ~90 lines of German prose).
      Kept deliberately: verbatim owner directives quoted in
      user.md/session.md/errors.md (they record what the owner said),
      the MIT licence string, and the v1 product's German CLI output
      (product identity, quoted by the docs; string-format gates).
      YAML re-validated; the C/Kotlin runtime strings stay German
      (documented scope decision).
- [x] R10-C: real model downloaded + verified. The sandbox network
      allows only github.com/codeload/api.github.com/npm/pypi — Hugging
      Face, raw.githubusercontent.com and the GitHub LFS endpoint are
      all unreachable, and GitHub stores model files >= 100 MB via
      LFS, so the "300 MB" target is not deliverable here (proven for
      gemma-3-270m-it-Q8_0 291.5 MB, qwen2.5-0.5b-instruct-q4_k_m,
      smollm2-135m — every copy found was an LFS pointer, 133/134 B).
      The largest real writing model obtainable: **llama-68m-chat-v1
      Q8_0, 73 018 688 B (73.0 MB)**, arch llama, 2 layers, dim 768,
      ctx 2048, 32k vocab, 21 tensors (16 Q8_0 + 5 F32), from
      BodhiSearch/BodhiApp (HF-cache layout, plain git blob) via the
      codeload tarball. Run on the PINNED llama-cli v0.5.0 (open
      stdin): "The capital of France is" -> "...France.<|im_end|>",
      prompt 109.4 t/s, generation 6.6 t/s (2 threads); longer prompt
      greedy 172.7 / 4.4 t/s; temp 0.7 generation 8.2 t/s. Honest
      ceiling: a 68M model writes thin prose; the pipeline (load,
      tokenize, full decode incl. Q8_0, sampling, detokenize, stream)
      is what is proven.
- [x] R10-D: the app path. LlamaBridge.load(path, nCtx) -> JNI ->
      `llama_model_load_from_file` (pinned v0.5.0) -> generate/
      cancel/unload: a thin wrapper, no size/arch/quant restriction of
      our own, architecture delegated to the pinned loader (the
      README claim, now backed by a real model). The model's ctx
      2048 fits the app clamp 256..8192. NOT measured (R32): the
      .so inside the APK on a device/emulator — the Android CI tier
      compiles the exact JNI source; the x86_64 emulator is the
      documented device-side path.
- [x] R10-E: the opt-in real-model gate. `tools/gguf_test.py
      --real-model <path>` (6 checks, off by default so CI stays
      offline + fast): magic bytes 47 47 55 46, full parse, every
      decodable tensor decodes, the converter projects it to .hydra
      v3 (dim 64, 2 layers, vocab 1024, 30 976 tokens dropped), the C
      engine runs the projection deterministically (two runs
      byte-identical, 16 tokens, non-degenerate). llama-68m: 107
      checks, 0 failed; the default set stays 101.
- [x] R10-F: docs — GGUF-IMPORT.md 11.7 (the full section: source,
      recipe, numbers, what each tool proved, what is not measured,
      the size ceiling + how a normal machine runs a ~300 MB model
      through the identical commands), README (GGUF bullet + tools
      tree), CHANGELOG round 10, Environment.md (model location +
      re-fetch recipe), user.md (directive 29). Gate sweep green,
      commit + push (PR #68 updated).


## Round 2026-10-26 (round 11: post-merge audit completion — JNI sampler config tests, web-console sampler, Android v2 header diagnostics, tf_row_dot SIMD)

**Session start (R33):** 2026-10-26 (branch `arena/476154f4-bestai`, base `fd1848a` = merge of PR #68). Read before the first edit: AGENTS.md (full), .agents/session.md (rounds 6-10), .agents/errors.md (top entries), .agents/user.md (directive list, tail), .agents/Environment.md (ARM/toolchain section: APT block, zig compile-only path, no qemu), .gitignore (safety check: .agents/ fully tracked, 23 files, owner-decision comment — NOT touched), the root inventory (no HYDRA2-RESEARCH.md at root — it lives at .agents/docs/HYDRA2-RESEARCH.md; no docs/HYDRA2.md — v2 format docs are docs/FORMAT-V2.md), the Makefile (full: targets, AVX2 probe, jni-batch-test deps), .github/workflows/ci.yml (jni-signatures job: jni_signature_check runs for BOTH bridges; android_header_check job), android/app/src/main/cpp/hydra_jni.c (full: 5 JNI exports, runInferenceSampled validation inline), android/app/src/main/cpp/hydra_batch.h (the single-source header pattern), tests/test_jni_batch.c (structure + includes), tools/jni_signature_check.sh (parameterised, negative-control via args), tools/android_header_check.sh (full check list), android/app/src/main/java/dev/hydrastone/MainActivity.kt (analyseHydraFile + describeMagic), android/app/src/main/res/values/strings.xml (err_magic_*), server.js (/api/infer handler, runInference, constants), public/app.js (run flow: POST /api/infer sends {path,prompt,steps,request_id} only), public/index.html (opt-* settings block), tools/server_test.js (structure, node:test), tools/hydra_train.js (deterministic hill climbing — no sampling), src/hydra_engine.c (M2 sampler, v3 step_tf, v4 hydra_transformer_forward_step, tf_row_dot scalar core, sampler step), src/hydra_simd.h + src/hydra_neon.h (existing AVX2/NEON kernels: accumulate_agg + v2 plane unpack), src/hydra_v2_container.c (open/magic, route_top_k, tensor types) + include/hydra_v2_container.h (HYDRA2_MAGIC_STR "HYDRA2\0\0", public API) + include/hydra_model.h (sampler ctx), android/app/build.gradle + android/CMakeLists.txt (hydra_v2_container.c is HOST-ONLY, not in the app build), tests/test_engine.c (M-E sampler suite, sampled-loop section, AVX2/NEON parity test patterns), tools/test_count_baseline.txt (x86 219 / arm 228, hydra2 122 separate).

**Owner directive (verbatim intent):** the post-merge audit from the owner: update test_jni_batch.c to also test the new JNI sampler configuration; the JNI signature gate should strictly cross-check all native methods across both bridges including a negative control; update tools/android_header_check.sh to verify include/hydra_v2_container.h alongside include/hydra_model.h; new CLI sampler flags (temperature, min_p, repetition penalty) (ALREADY PRESENT in src/main.c — verified: --temp/--min-p/--rep-penalty/--rep-window with validation; documented as such, not re-implemented); web console: new controls for temperature/min_p/rep_penalty + new sampler flags for the training backend if applicable (trainer is deterministic hill climbing — no sampling — documented as N/A with evidence); SIMD the v2-container expert decode paths and the new format v4 (evidence-based scope: tf_row_dot is the v3/v4 forward's scalar integer-exact core -> AVX2+NEON; the container's expert decode is a documented O(k^2 n) comparison-based selection + double softmax with no data-parallel arithmetic -> documented, not changed); update the docs to reflect the new features. PLUS the owner's emphatic reply (2026-10-26): "Mache so, dass du gewissenhaft weißt, dass du nichts kaputt machst! Also gitignor und die wichtigsten Agenten Files session.md etc. IMMER EGAL WAS ICH SAGE wahre und beschützt und sie stets aktualisierst!! Bitte prüfen und sicher fortfahren!" — STANDING RULE for this round (highest priority after safety): .agents/ files (session.md, errors.md, user.md, Environment.md, rules.md, docs/) are SACROSANCT — never delete them, never add them to .gitignore, keep them current in every round, regardless of any other instruction; .gitignore itself is not touched. Proceed thoroughly: legacy behaviour stays bit-identical (greedy default unchanged, v1 baseline stream untouched, all existing gates green) and every local gate runs before any commit.

- [x] R11-A (FERTIG): JNI sampler config — extract runInferenceSampled's validation into
      android/app/src/main/cpp/hydra_sample_cfg.h (the hydra_batch.h single-source
      pattern: one header compiled into both libhydra.so and the host test);
      hydra_jni.c calls it (messages + check order unchanged); new section in
      tests/test_jni_batch.c (boundaries: temp 0/20.0, minP 0/1.0, rp 1.0/2.0
      accepted; temp -0.1/20.001/NaN/Inf, minP -0.001/1.001, rp 0.9999 rejected;
      greedy-default proof); Makefile jni-batch-test dep + new count;
      tools/sampler_contract_check.sh (cross-surface: main.c CLI, JNI header,
      server.js share the same range rules; negative control) wired into the
      ci.yml jni-signatures job; tools/jni_signature_negative_test.sh (missing
      negative control, pattern: android_autostart_negative_test.sh).
- [x] R11-B (FERTIG): web console sampler — server.js /api/infer accepts
      temperature/minP/repetitionPenalty/repWindow, validates with the SAME
      rules as the JNI header (400 + clear message, non-finite rejected),
      passes --temp/--min-p/--rep-penalty/--rep-window as execFile array args
      (no shell); legacy request (no fields) emits NO flags -> greedy -> the
      existing 37 server_test.js cases stay green bit-identical; index.html +
      app.js: sampler controls (defaults = off = legacy behaviour);
      server_test.js new live HTTP tests (legacy bit parity, temp=0 == greedy
      stream, temp>0+seed changes the stream = no-op negative control, 400 for
      every invalid value/type); trainer (tools/hydra_train.js) untouched —
      deterministic hill climbing has no sampling stage (documented).
- [x] R11-C (FERTIG): Android v2 header diagnostics — a .hydra2 file (magic
      "HYDRA2\0\0": first 4 bytes are the SAME "HYDR" as v1, the tail
      "A2\0\0" distinguishes) currently reports "magic matches .hydra" + a
      generic version error; MainActivity.kt names the HYDRA2 container by
      name (strings.xml entry, mirroring the v2 reader's own refusal);
      tools/android_header_check.sh gains the v2 checks INCLUDING a
      cross-surface check that the Kotlin constant equals bytes 4-7 of
      HYDRA2_MAGIC_STR in include/hydra_v2_container.h (the v2 header path
      verified alongside the v1 header path, as audited).
- [x] R11-D (FERTIG): SIMD — tf_row_dot (the 2-bit ternary x int8 dot that v3 step_tf
      and v4 hydra_transformer_forward_step call ~384x/token) gets
      hydra_avx2_tf_row_dot (src/hydra_simd.h, 128-bit: zero-padded 16-byte
      load + pshufb unpack x4 + maddubs + int32 sum — INTEGER EXACT, so the
      pinned streams stay valid) and hydra_neon_tf_row_dot (src/hydra_neon.h,
      vtbl1 + vmlsabdq); dispatch in hydra_engine.c (AVX2 runtime probe as in
      M3, NEON compile-time as in the load path); parity tests in
      tests/test_engine.c (x86: LCG-random rows/vectors over dims 4..64 +
      all-code sweep + extremes vs an independent scalar reference, the M3
      pattern; NEON: same, compiled-only locally per Environment.md (no qemu),
      executed on the ARM CI leg); tools/test_count_baseline.txt x86/arm lines
      updated in the SAME commit (R21). Container SIMD: NOT done, with the
      evidence (route_top_k is comparison-based selection + double softmax,
      the container has no data-parallel arithmetic, plane unpack is already
      AVX2/NEON); float ops (rmsnorm/rope/swiglu/attention) stay scalar
      (bit-exact pinned streams).
- [x] R11-E (FERTIG): docs — AGENTS.md (v3/v4 transformer + per-layer KV, .hydra2
      container, sampler on all surfaces, SIMD map, the two new gates),
      README.md (CLI sampler flags if not already documented), CHANGELOG.md
      round 11, errors.md (as they happen), user.md directive 30.
- [x] R11-F (FERTIG): gates + PR — the full 5-part local gate (make clean && make &&
      make test && make test-run; static analysis; python; node; guardrails),
      v1 baseline stream [471 386 385 386 385 386 385 386] unchanged,
      docs_claim_check, test_count_check vs the new baseline, zig aarch64
      compile check of the NEON path (Environment.md's recorded substitute),
      then commit on arena/476154f4-bestai, push, PR with facts only, CI green
      including the ARM leg.

### R11 results (measured, 2026-10-26)

- **R11-A:** `hydra_sample_cfg.h` (single source: ranges temp 0..20 / minP
  0..1 / rp >= 1.0 / window 0..4096, isfinite, order temp→minP→rp, codes
  0–3, defaults 0/0/1.0/64). jni-batch-test **109 checks, 0 failed** (was
  86). `sampler_contract_check.sh` **34/34 GRÜN** + negative control rc=1
  (drift "C header '25.0' vs server.js '20'"). `jni_signature_negative_test.sh`
  local SKIP rc=0 (no javac in sandbox — said so), S1 phantomMethod /
  S2 load-Arität / sed-Guard; CI proof on JDK 17. Both wired into the
  ci.yml jni-signatures job; `workflow_yaml_check.sh` green (5 workflows).
- **R11-B:** server.js strict validation (`typeof v !== 'number' ||
  !Number.isFinite(v)`, window additionally integer); field-less request
  appends NO flags (legacy bit-exact). ECHTER BUG gefangen vom eigenen
  Test: `Number(null) === 0` ließ NaN-serialisiertes Feld als temp 0
  durch — jetzt 400. server_test **43/43** (was 37). `hydra_train.js`
  unangetastet (deterministisch, dokumentiert).
- **R11-C:** MainActivity.kt `HYDRA2_TAIL_U32 = 0x00003241L` +
  HeaderRule("format", …) + `err_magic_hydra2` (strings.xml).
  android_header_check **28/28 GRÜN** (was 21) inkl. 2 computed
  cross-surface pins (python3 rechnet die Kotlin-Konstanten aus den C
  Magic-Strings) + Negativ rc=1 (0x00009999L). R_gen.kt-Stub →
  kotlin_stub_check OK 0 errors.
- **R11-D:** AVX2- + NEON-Kernel für `tf_row_dot` (Design: 1 Row-Byte =
  4× 2-Bit-Codes ⇒ 8-dim-Chunk = 2 Row-Bytes + 8 x-Bytes; T3={0,1,-1,0};
  integer-exakt, Lane-Bound 2·127, Total-Bound 64·127=8128 ⇒
  bit-identische Streams, kein Re-Pinning). Parity:
  `/tmp/par_check.c` (recreatable: gcc -O2 -mavx2 -Iinclude,
  scalar-Referenz + LCG, dims 4..64 × 2000, x exakt dim groß) **ALL OK**
  + **ASan-CLEAN** (x exakt dim groß). `hydra-test` **225/225 x86**
  (was 219; +6 AVX2-Parity-Checks); ARM-Baseline 228 → **234** (progn.
  +6 NEON-Checks, CI-ARM-Leg misst). Pinned v3/v4-Streams gültig (die
  beiden Stream-Tests haben die erste Kernel-Version gefangen). NEON:
  zig-aarch64-Full-Compile (engine + tests) rc=0, kein lokaler ARM-Run
  (kein QEMU — Environment.md-Rezept; CI neon-test = Ausführungsbeweis).
- **R11-E:** AGENTS.md §2.1 (v3/v4 + SIMD-Dispatch + dokumentierte
  Scope-Entscheidungen Container-Top-k/Floats), §3 (jni 109, server 43,
  beide neuen Gates), README (CLI-Sampler-Flags + Contract),
  CHANGELOG R11, docs/FORMAT-V2.md (SIMD-Status-Punkt).
- **R11-F:** 5-Teil-Gate lokal GRÜN (alle Suites, Linter — inkl.
  cppcheck 2.13.2 aus Quell-Build, exakter CI-Befehl, 0 Warnungen;
  Python, Node, alle Guardrails, actionlint, session-check). v1-Legacy:
  die [471 386 …]-Referenz ist ein Session-Log-Wert eines alten Modells
  und KEIN Code-Pin (grep belegt: nur in session.md); der relevante
  Proof ist die Bit-Identität gegen den Base-Commit-Build — gemessen:
  v1-Stream (normal UND --fast, 32 Tokens) byte-identisch Base vs.
  Branch. Commits e157480 + 5142045 (NULL-Checks, CI-cppcheck-2.13
  nullPointerOutOfMemory) + 6b0797a (row[16]={0}, CI-cppcheck-2.13
  uninitvar False-Positive, mit 2.13.2-Quellbuild reproduziert und
  danach rc=0). PR #70 geöffnet (FACTS ONLY). CI auf dem PR: **16
  passed / 0 failed**; 4 Jobs (neon-test QEMU, build-test, Android
  APK, GPU bench) by design skipping im PR — Owner-Regel: PR = Fast-
  Lint, main = voller Build. Der NEON-Runtime-Beweis (test_neon_
  tf_row_dot, 234er-Baseline) läuft nach dem Merge auf main.
  /tmp-Resets in dieser Runde: zvenv + lintenv neu aufgebaut
  (Rezepte Environment.md); pyyaml via pip --break-system-packages;
  llama.cpp-Submodule per `git submodule update --init` auf den Pin
  ausgecheckt (sonst pin-check "working tree"-Meldung).
