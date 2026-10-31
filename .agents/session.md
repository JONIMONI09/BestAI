# Session Log — Hydra-Stone

> Protocol: see `.agents/skills/session-workflow/SKILL.md`.
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

(rounds 6-11, 2026-10-26, archived verbatim on 2026-10-30 -> .agents/docs/SESSION-ARCHIVE.md)

## Round 12 (2026-10-27) — audit of issue #72 ("post-fd1848ac transformer + .hydra2 expansion")

**Owner directive (verbatim):** "ANALYSIEREN? … Bitte prüfen ob das Stimmt dann Doku machen und so etc !" with issue https://github.com/JONIMONI09/BestAI/issues/72 attached (6 milestones M1-M5 + a verification list). The issue is a post-merge audit drafted BEFORE PR #70 was merged — several of its claims are already obsolete. Task: verify every claim against the merged code (evidence-based, like the R11 audit), document the findings, then plan what is genuinely new.

**Session start (R33):** 2026-10-27 (branch `arena/44edb923-bestai`, base `94f8e08` = merge of PR #70). Read before the first edit: AGENTS.md (full, incl. the new §2.1 v3/v4+SIMD paragraph and §3 counts), .agents/session.md (rounds 8-11, esp. R11 fact-finding + results), .agents/errors.md (R11 SIMD bug series + Number(null) entries), .agents/rules.md, .agents/user.md (directive 30 + standing rules), .agents/Environment.md (round-11 block: /tmp resets, pyyaml, submodule checkout, CI cppcheck 2.13 version gap, log-fetch block), android/app/src/main/cpp/CMakeLists.txt (full: HYDRA_ROOT = 5 levels up; libhydra.so = hydra_engine.c + hydra_jni.c ONLY; llama.cpp separate .so), android/app/src/main/cpp/hydra_jni.c (4 JNI exports: cancel/runInference/runInferenceSampled/benchmark; NO hydra_v2_container.h include; L319 comment: v3 dispatch = step_tf + caller KV), android/app/src/main/java/dev/hydrastone/HydraBridge.kt (runInference/runInferenceSampled/benchmark/cancel; runInference "still uses the v1/v2 step and refuses v3"), tests/test_jni_batch.c (109 checks; ZERO v3/v4 coverage - grep v3/transformer = empty), tools/jni_signature_check.sh (parameterised per bridge, name-based), include/hydra_v2_container.h (full: "it does not run MoE inference, it does not implement an expert cache, and it does not map the trunk"; tensor roles TRUNK/ROUTER/EMBEDDING/NORM/KV/SHARED_EXPERT; types incl. F16/BF16; archs hydra-recurrent-v1 + moe-transformer-v1), src/hydra_v2_container.c (metadata = routing K/V pairs: top_k etc.), include/hydra_model.h (v3 fixed design params L24-37; v4 pass L175-215: single full-dim causal attention, NO n_heads/n_kv_heads, NO MoE, w1 token injection, W_out; sampler = ONE acc[0] accumulator, no per-vocab logit vector), src/main.c (--prompt = token IDs only, NO --text-prompt, NO text decode), tools/gguf_to_hydra.py ("the vocabulary cannot be stored" L324), tools/gguf_minimal_llama.py (exists: minimal real llama GGUF + BPE-style tokenizer for the llama.cpp pipeline, --out required), server.js (NO .hydra2 handling), docs/FORMAT-V2.md (implementation status: container reader only, no inference path), tools/test_count_baseline.txt (x86 225 / arm 234 post-PR#70).

### R12-A: evidence-based verdict per issue claim (measured 2026-10-27)

DONE (merged in PR #70, 2026-10-26):
- M1.3-part: sampler-config JNI tests (jni-batch 109 incl. boundaries/rejections/defaults);
  jni_signature_negative_test.sh (both bridges in CI + negative control);
  android_header_check .hydra2 validation (28 checks incl. computed
  cross-surface pins). Measured post-merge: 109/109, 28/28, hydra-test
  225/225.
- M2.1-part: tf_row_dot (the v3/v4 projection hot path, 384 calls/token)
  has AVX2 + NEON kernels with scalar fallback (PR #70); parity tests
  225/234; integer-exact => bit-identical streams.
- M5-part: web console sampler flags (server_test 43), AGENTS/README/
  CHANGELOG/FORMAT-V2 docs, test_count_baseline same-commit update.
- The issue's invariant is CORRECT and measured: starter model,
  start_token=42, greedy, 8 steps -> [471 386 385 386 385 386 385 386].

STALE/WRONG in the issue:
- M1.1: "add ../../../../src/hydra_v2_container.c to CMakeLists" —
  UNNECESSARY AND THE PATH IS WRONG. (a) CMake defines HYDRA_ROOT 5
  levels up (`${CMAKE_CURRENT_SOURCE_DIR}/../../../../..`), so
  `../../../../src/...` resolves to android/src/... (nonexistent).
  (b) libhydra.so compiles hydra_engine.c + hydra_jni.c only, and
  NEITHER references a single container symbol (grep: the only
  hydra2 mention in hydra_engine.c is a refusal message L375) — adding
  the .c would ship dead code in the phone .so. The container is
  deliberately host-only (FORMAT-V2.md implementation status: "there is
  no code path in hydra-run that consumes a .hydra2 file").
- M1.3-rest: "test all new JNI entry points (.hydra2 loading)" — there
  is NO .hydra2 JNI entry point (the bridge exports exactly
  cancel/runInference/runInferenceSampled/benchmark; hydra_jni.c has no
  container include). The audit asks for tests of a feature that does
  not exist. PARTIALLY VALID: the v3/v4 transformer path IS reachable
  via runInferenceSampled (documented at hydra_jni.c L319), but
  tests/test_jni_batch.c covers it ZERO times (grep v3/transformer =
  empty) — that gap is real and testable.
- M2.2: per-group FP16/FP32 block scaling (group 64/128) — does not
  exist. What DOES exist: one u16 absmean scale PER LAYER
  (HYDRA_TF_SCALE_FP, hydra_model.h L33). Per-group scales = new
  format work; retro-applying them would break the pinned streams, so
  it must be additive (new version / new tensor type).
- M3.1: GQA (n_heads/n_kv_heads), Sparse-MoE FFN, token-embedding
  lookup, separate lm_head — NONE exist in v4. v4 is fixed-design:
  single full-dimension causal attention (one Wq/Wk/Wv per layer, no
  head split), dense ternary SwiGLU FFN, w1 token injection (NOT an
  embedding table), W_out as the logit projection. The issue conflates
  the container's STORED tensor roles (EMBEDDING/NORM/KV for the
  future moe-transformer-v1 arch) with the executable v3/v4 engine —
  the container explicitly does not run inference.
- M3.2: mmap for .hydra2 — the container explicitly does NOT map the
  trunk (documented scope, header L10-13); it uses pread-per-expert +
  hydra2_read_tensor() for the trunk. New feature (would reverse a
  documented design decision).
- M4: no format stores token strings (v1 header = vocab_size only;
  v2 roles have no VOCAB entry; gguf_to_hydra.py L324: "the
  vocabulary cannot be stored"); no --text-prompt and no BPE in
  main.c (token IDs only). NEW feature set (format + C + Python).
- M5-rest: .hydra2 inspection in the web console — server.js has no
  .hydra2 handling. NEW (small-medium).

WRONG in the issue's verification list (measured):
- `python3 tools/gguf_minimal_llama.py` (no args) -> usage error,
  --out is required.
- `bash tools/test_count_check.sh` (no args) -> "FAIL: usage:
  test_count_check.sh <binary> <arch-label>".
- `node tools/server_test.js` works but the repo convention is
  `node --test tools/server_test.js`.
- `tools/compose_scroll_guard.py` exists but the WIREd gate is
  tools/compose_scroll_check.sh (AGENTS.md §3).
- Existing + fine: apple_portability_check.sh,
  kotlin_stub_check/kotlin_stub_check.sh, session_start_check.sh,
  ci_syntax_mirror_test.sh, workflow_lint_check.sh, workflow_yaml_
  check.sh, llama_pin_check.sh, docs_claim_check.sh,
  android_header/autostart/crash_check.sh, jni_signature_check.sh.

### R12 plan (owner approved 2026-10-27: "alles fixen was geht!!!!" = full scope)

DESIGN (all additive; legacy bit-identical; new identifiers):
- NEW tensor roles (reader updated, old readers refuse new files =
  documented compat rule): 7=ATTN (attn_q/k/v.{l}), 8=VOCAB ('vocab'
  type 0x0050 VOCAB_UTF8, 'merges' type 0x0051 U16_PAIRS), 9=LM_HEAD
  (optional; default = tied embedding, llama-style).
- NEW tensor type ids (FORMAT-V2.md 3.2 extension): 0x0040
  TERNARY2_S64, 0x0041 TERNARY2_S128 (ternary2 payload + paired
  F32 '<name>.scale' tensor, n = rows*ceil(cols/group); per-group
  int32 partial dot (bound G*127) * f32 FMA); 0x0050/0x0051 above.
- NEW inference metadata (enforced by the MoE layer, container stays
  structural): v2.dim, v2.vocab_size, v2.n_heads, v2.n_kv_heads,
  v2.head_dim, v2.inter, v2.expert_layout (0=F32 flat fixture,
  1=ternary+scales [gate|up|down]), v2.rope_theta, v2.kv_sink,
  v2.kv_ring, v2.tied_embedding, v2.bos_token.
- NEW module src/hydra_moe.c + include/hydra_moe.h: open on container
  (mmap trunk + pread fallback), full per-layer GQA pass (F32
  attention, RoPE theta 10000, per-(layer,kv_head) sink+ring KV),
  router -> top-k -> streamed expert FFN (F32 or ternary+scale),
  final RMSNorm, logits via tied embed (or LM_HEAD), REAL per-vocab
  logit vector -> new min-p/rep sampler (v1 sampler untouched, v1
  streams stay bit-identical).
- main.c: magic dispatch (.hydra2 -> MoE engine), --text-prompt for
  .hydra2 (BPE encode from embedded merges; strict), UTF-8 decode of
  output tokens, --inspect for both formats (JSON report).
- server.js: /api/models/inspect + list handle .hydra2 (magic
  dispatch), UI + server_test.
- tools/make_model.py: --moe-full (complete-contract fixture incl.
  attention/scales/vocab); legacy --v2 fixture UNCHANGED (122 checks).
- NEW tools/gguf_to_hydra2.py: GGUF -> .hydra2 moe-transformer-v1
  (real vocab+merges, F32 embed, ternarized attention, dense-FFN ->
  E-expert slice surgery, documented approximation, NOT a bit-exact
  GGUF port).
- JNI gap: v3/v4 coverage in test_jni_batch.c via the same C logic the
  bridge calls (repo pattern: shared header, cf. hydra_batch.h).
- ANDROID decision (documented): .hydra2 inference stays host-only
  this round (R23/R32: new kernels need the NEON port + ARM runtime
  proof; no dead code into the .so -> M1.1 remains "no, wrong path").
- Gates: every SIMD variant parity-tested scalar vs SIMD (x86+ARM
  baselines, ASan, zig), deterministic stream pins, same-commit
  test_count_baseline, cppcheck 2.13.2 replication before push.

- [x] R12-A: evidence-based audit of issue #72 (above).
- [x] R12-B: documentation — .agents/docs/ISSUE-72-AUDIT.md (the full
      verdict table with file:line evidence), session.md (this
      section).
- [x] R12-C: scope decision — owner: "alles fixen was geht!!!!" (fix everything that can be fixed).
- [ ] R12-D: implement — stage 1 (JNI v3/v4 batch gap + inspect),
      stage 2 (mmap), stage 3 (scales + kernels), stage 4 (MoE
      engine F32), stage 5 (ternary+scale experts), stage 6
      (vocab+BPE+converter), stage 7 (docs/gates/PR).
- [ ] R12-E: full local gate (incl. CI cppcheck 2.13.2 replication),
      commit, push, PR facts-only.

## Round 12 progress (2026-10-30, continuation) — stages 1-7 state

**Session start (R33):** 2026-10-30 (branch `arena/44edb923-bestai`,
base `94f8e08`, R12 audit committed at `c992cd6`). Continuation
session resumed from the condensed session state; read before the
first edit of this continuation: .agents/session.md (round 12
section + this plan), .agents/errors.md (top entries),
.agents/rules.md, docs/FORMAT-V2.md (3.2.1/3.2.3 + implementation
status), tools/gguf_test.py + gguf_reader.py (reader API, decode_rows
flat semantics, Tensor.shape 1-D = (1, n)), tools/gguf_minimal_llama.py
(tensor names of the pinned build: token_embd.weight /
attn_output.weight / output_norm.weight, GGUF linears stored
[in, out]), tools/make_model.py (_assemble_v2 + build_moe_full meta),
tests/test_hydra_moe.c (ref_forward engine conventions),
Makefile (test-run), ci.yml (gate layout).

All work on branch arena/44edb923-bestai. Owner: "alles fixen was
geht!!!!" (fix everything that can be fixed) + "mache schneller bitte
weiter, und doku am ende nicht vergessen!!!" (keep going faster, and
don't forget the docs at the end) (2026-10-30).

- [x] R12-D stage 1: JNI v3/v4 batch gap closed. tests/test_jni_batch.c
      +19 (109 -> 128): deterministic v3/v4 writers, batched-run ==
      direct-run stream equality (greedy AND fixed-seed sampling), v4
      stream pin, token-0 fixed point, 17-step partial block.
- [x] R12-D stage 2: trunk mmap (src/hydra_v2_container.c): resident
      trunk region mapped when possible (trunk_map + page-aligned base
      offset), pread stays the portable fallback; tensor reads serve
      from the mapping. test_hydra2_container.c now 152 (was 144
      after the vocab work; the mmap section pins both backends).
- [x] R12-D stage 3: block-scaled ternary M2.2. Types 0x0040/0x0041
      (TERNARY2 + per-group F32 scales, '<name>.scale' pairing,
      FORMAT-V2.md 3.2.1 with ceil(n/G)); hydra_tf_row_dot_scaled
      (integer-exact group partials * f32 FMA); +9 main-suite checks
      (234 x86 / 243 arm baseline updated in
      tools/test_count_baseline.txt).
- [x] R12-D stage 4: MoE inference engine M3.1 (src/hydra_moe.c +
      include/hydra_moe.h, NEW): open on .hydra2, per-layer GQA pass
      (F32 attention, partial RoPE from v2.rope_theta, per-layer
      sink+ring KV, NON-CAUSAL over cached positions = documented
      deviation), router -> deterministic top-k -> streamed expert
      FFN, real per-vocab logits, min-p/rep sampler. main.c magic
      dispatch. tests/test_hydra_moe.c NEW: 138 checks (parity vs
      independent reference, KV pins, tamper refusals, ternary dequant
      unit vs independent reference).
- [x] R12-D stage 5: ternary + block-scale expert layouts (layout 1:
      TERNARY2_S64/S128 experts [gate|up|down]); heap-overflow fix in
      moe_get_tensor (alloc by destination width, not source);
      attention-ternary trunk path (attn_q etc. block-scaled,
      --moe-attn-ternary fixture, 8288 B); AT64 pinned streams
      "10 5" x 12.
- [x] R12-D stage 6: vocab + BPE + CLI + converter.
      6a/6b/6c: role-8 tensors (VOCAB_UTF8 0x0050, U16_PAIRS 0x0051),
      moe_* codec API in hydra_moe.c (strict BPE, byte-level tokens,
      loud refusals: duplicate tokens, out-of-range rule ids, missing
      concat target, missing trailing NUL), web console inspection of
      .hydra2 (server.js /api/models/inspect, public/app.js,
      server_test.js 47), make_model.py --moe-full vocab/merges.
      6d: container hardening (vocab/merges n_bytes rules, tamper
      fixtures in test_hydra2_container.c -> 152).
      6e: hydra-run --text-prompt (main.c): BPE-encode user text with
      the embedded vocab, parity with numeric --prompt pinned, JSON
      "text" echo, [Text] line, mutual exclusion, refusals (v1,
      vocab-less, bench combo); moe_find_quiet() for optional tensor
      lookups. Gate: tools/text_prompt_check.sh (ci.yml gate 1h).
      6f: tools/gguf_to_hydra2.py NEW (M2.2/M3.1/M4): llama GGUF
      (official AND pinned-build tensor names, F32/F16/BF16/Q8_0) ->
      complete-contract .hydra2 moe-transformer-v1: transposed
      linears (GGUF [in,out] -> engine [out,in]), uniform router
      (all-zero rows -> softmax 1/E), E full-width experts partitioning
      the intermediate rows (gate/up masked + xE, down full) so the
      engine result equals the dense FFN up to F32 order rounding;
      E=1 default (verbatim dense FFN as one expert), --max-experts
      bound (cost: E x FFN storage per layer, documented); embedded
      vocab 1:1, merges optional (rules whose concat is not a token
      are skipped, documented); rope_theta from llama.rope.freq_base;
      loud refusals (non-GGUF, non-llama, undecodable tensor, missing
      tokens, dim mismatch). Gate: tools/gguf_to_hydra2_test.py NEW
      (26 checks: engine determinism, expert CRC tamper refusal,
      F32-exact embed transpose, F32-exact masked-expert identity,
      byte-exact vocab, metadata, router zeros, merges roundtrip,
      5 refusal controls). Makefile gguf-conv-test in test-run.
- [x] R12-D stage 7 (partial, this entry): docs + gates + sanitize.
      * FORMAT-V2.md: 3.2.1 (ceil + trunk ternary materialization),
        3.2.3 (vocab/merges codec + converter + documented
        approximations), 7 type rows, implementation-status bullet
        (round 12 stages 4-6, 138 checks, codec refusals, fixture
        list), Q8_0 row, cross-ref fix.
      * UBSan finding 2026-10-30: misaligned float load in the
        ternary expert scale read (n % 4 == 2) - fixed with memcpy in
        src/hydra_moe.c + test reference; NEW CI step "Sanitizers
        (ASan+UBSan) over the whole test suite" (build-test job, both
        matrix legs, UBSAN_OPTIONS=halt_on_error=1). errors.md entry.
      * ci.yml: lint job now builds ./hydra-run before the binary-
        driven gates (gate 1h needed it but the lint job never built -
        would have failed in CI); converter gate added (gate 1i);
        numbering fixed (1h/1i -> 1j/1k for yaml/lint gates).
- [x] R12-E: full local gate pass, commit e99fc30 (31 files,
      +7141/-93), pushed, PR #73 opened (facts-only body). All gates
      green at push time (state above); kotlin_stub ran with the
      npm-installed kotlinc 2.4.21 (0 errors); the pinned llama.cpp
      v0.5.0 submodule was checked out locally for the pin gate.

Verification state at this entry (local, 2026-10-30):
- make clean && make && make test: green. make test-run: hydra-test
  234/0 (baseline x86 234), jni-batch 128/0, large-model 7/0, gguf
  101/0 (v1 gate UNTOUCHED, byte-identical fixtures), gguf-conv 26/0,
  hydra2-test 152/0, hydra-moe-test 138/0.
- node --test tools/server_test.js: 47/0 (a stale local `node
  server.js` caused one port-conflict flake; killed, re-ran clean x3).
- ASan+UBSan (halt_on_error=1): all five test binaries green, 0
  runtime errors (after the misaligned-load fix).
- gcc -fanalyzer (C99, -Wall -Wextra -Werror): clean on all 4 TUs.
- cppcheck 2.13.2 exact CI command: NOT runnable in this sandbox
  (2026-10-30: the repo-proxy whitelist no longer serves
  cppcheck/cppcheck - codeload + git + api all 404 - and the sandbox
  has no root for apt). The CI gate (lint job, apt cppcheck) covers
  it; local static analysis was done via -fanalyzer + ASan/UBSan
  instead. Re-run locally when the tool is reachable again.
- .sh gates: run in the final pass (session_start, jni_signature,
  kotlin_stub, compose_scroll, android_header/autostart/crash,
  apple_portability, ci_syntax_mirror, workflow_yaml, workflow_lint,
  llama_pin, docs_claim, test_count x86).

**CI triage (2026-10-30, after the R12 push) — both workflows GREEN
at `c5fcd38`, PR #73 ready for merge:**
- CI run at 22a67c5 failed in the lint job with ONLY "Process
  completed with exit code 2" visible: `gh run view --log-failed` and
  artifact download both hit the sandbox network whitelist (results-
  receiver / blob storage EOF), and the jobs API returned an empty
  step list. Triage path that worked: (1) the check-run ANNOTATIONS
  API (`/check-runs/<id>/annotations`) revealed the failing line
  (ci.yml:69) + exit code 2; (2) exit code 2 was the signature of the
  cppcheck gate (`--error-exitcode=2` — Python failures exit 1);
  (3) a temporary diagnostic step (same cppcheck command,
  `--error-exitcode=0`, every line re-emitted as a `::error::`
  annotation) made the actual findings visible through the same API.
  Keep this recipe for any future red-gate-with-unreadable-logs.
- Findings (cppcheck 2.13.2, `--enable=warning,portability`):
  1. `tests/test_hydra_moe.c:304` `[negativeIndex]` — the reference
     top-k used a `best = -1` sentinel; if the expert count were 0 the
     `rk[best]` write would index -1. Fixed structurally: `best = 0`
     start (epl >= 1 is guaranteed by the model config) + strict `>`
     scan — identical winner selection (tie -> lower id), 138/0 after.
  2. `src/hydra_moe.c:1181` `[invalidPointerCast]` (portability) —
     the intentional `uint8_t* -> const float*` view of the F32 expert
     scratch buffer. Documented + inline suppression (the gate is run
     with `--inline-suppr`; buffer is malloc-aligned, payload F32 by
     format contract).
  cppcheck aborted at 82% on the first error-severity finding, so the
  second round of annotations (after both fixes) ran to 100% with zero
  findings — the remaining files were clean. The temporary diagnostic
  step was removed in the final commit; the lint job is back to its
  original shape.
- Final state at `c5fcd38`: CI workflow SUCCESS (lint + JNI bridge +
  llama pin + Portability; build-test/neon/GPU/Android skipped on PR
  by design — they are `if: refs/heads/main`-only per the PR-vs-main
  CI split) and Lint & Security 11/11 SUCCESS (incl. ASan+UBSan
  runtime, CodeQL c-cpp, Semgrep). The heavy main-only jobs will run
  the full suite when the owner merges to main.

## Round 13 (2026-10-30) — Qodo/CodeQL audit of PR #73 @ c5fcd38: every finding re-verified, then fixed with regression tests

**Session start (R33):** 2026-10-30 (branch `arena/44edb923-bestai`, base commit 9f2691d, PR #73 open). Read before touching code: `.agents/README.md` -> `session.md` (round 12 + archive protocol) -> `rules.md` (R1-R33) -> `user.md` (owner directives) -> `Environment.md` (sandbox capabilities + recreate commands) -> `errors.md` (top entries) -> `skills/repo-docs-audit/SKILL.md` + `skills/session-workflow/SKILL.md` -> the Qodo review comment of PR #73 (issue #74, all 30 findings) + the linked CodeQL alert (copy_file 0666, test_hydra_moe.c) -> `docs/FORMAT-V2.md` (implementation status) -> the round-12 audit file `.agents/docs/ISSUE-72-AUDIT.md` for context.

**Owner directive (2026-10-30, verbatim intent):** with the Qodo review comment (issue #74 -> PR #73, commit c5fcd38) + the linked CodeQL alert: "schaue dir bitte mal alles an, so was findings etc angeht, ob es wirklich gefixt wurde und oder noch offen ist" — audit all 30 Qodo findings (13 H / 13 M / 4 L + 1 resolved) + CodeQL, verdict per finding, then fix what is open. Tempo + docs at the end: "mache schneller bitte weiter, und doku am ende nicht vergessen!!!"

**Audit verdict (measured against the code, all 29 open findings CONFIRMED, 0 false positives; the 1 CodeQL alert confirmed too):** 26 Qodo findings fixed (23 code + 3 doc-only: #10 archive rule, #14 machine names, #29 English owner quotes) + the 1 CodeQL alert fixed (code) = 27 closed, 3 explicit deferrals (#11 JNI-batch coverage of the .hydra2 MoE engine = separate milestone, #12 SIMD dispatch for MoE matvecs = benchmark decision, #13 zero-copy expert streaming = format-level decision after the #3 packed-buffer contract).

**Fixed this round (per finding, file + evidence):**
- Converter #1/#2/#4/#9/#15/#24: file-order copy, expert scaling, strict token normalization, U16 overflow refusal, norm-shape refusal. Gate 26 -> 38 checks green.
- #3 ternary in-place aliasing: separate packed buffer in `moe_get_tensor` + public `hydra_moe_dequant_flat`; unit test with the corruption replay (first corrupted value = element 4); AT64 stream re-pinned (`12 10 10 10 5 10 10 5 10 5 10 5 10 5 10 5 10 5 10 5 10 5 10 5` — the pre-fix pin was produced BY the bug). Packing fact corrected everywhere: TERNARY2 = 2 elems/byte -> n/2 bytes.
- #8 temperature cancellation: `expf((adj-mx)/T)` (pre-fix `expf(adj-mx)/T` + renorm cancelled T); 3-temperature CDF pin (T 0.5/1.0/2.0 -> tokens 0/1/2 for one seed).
- #16 ring slot-0 freeze + #22 window-0 SIGFPE: true circular buffer (monotonic write counter), `--rep-window 0` safe; both pinned in `text_prompt_check.sh` (wraparound stream `0 6 0 6 0 6 2 7`, pre-fix `0 6 0 6 0 6 0 6`).
- #17 32-bit shape wrap: 64-bit products + tamper test (n_heads = 2+2^30 wraps to dim 8, refused -7).
- #18 inspect JSON double-quoting: `meta_value_json` returns COMPLETE JSON values, call site heuristic removed.
- #19 NULL rng: stochastic path refuses (greedy may pass NULL).
- #20 UTF-8 error@0: validator returns a distinct code + offset; offset-0 refusal test (pre-fix it died as "unmapped byte").
- #21 window cap: penalty walks the whole window; pin proves repeats beyond the vocab cap are penalized.
- #23 .hydra2 dispatch before defaults: defaults/clamp + prompt-wins now precede the magic dispatch (bare `hydra-run model.hydra2` worked pre-fix only by accident); JSON pin (start 42->10, 16 tokens).
- #25 inspect path echo: refused read prints a fixed message, no path.
- #26 GQA interleave: block mapping `i / (nh/nkv)` (llama.cpp convention) + `nh % nkv == 0` open refusal + new GQA fixture (nh 4 / nkv 2) with stream pin `15 9 12 12 9 13 13 13 13 13 10 13` (pre-fix interleave: `12 12 12 12 12 12 5 5 5 5 13 5`).
- #27 server execFileSync: async `execFile` (event loop no longer stalls); the async error carries the exit code in `.code`, probed alongside `.status`.
- #28 KV-cache dim: forward refuses a cache whose vector length != n_kv_heads*head_dim.
- #30 prompt prefill: prompt tokens 0..n-2 run through the engine (KV history), generation continues from the LAST prompt token; pre-fix the prompt only seeded the repetition ring. The R12 text/numeric parity now holds BECAUSE both paths prefill the same tokens.
- #5 trunk_map_len: the mapped extent is remembered; `hydra2_close` munmaps the exact range. Enabled `detect_leaks=1` in CI after all five suites ran LSan-clean locally.
- #7 decoded output: `[Text-out]:` line + JSON `generated_text` field (engine has the decoder, the CLI never called it).
- CodeQL C1: test `copy_file` creates its /tmp fixtures 0600 via `open(O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW)` + fdopen.
- #10/#14/#29: session.md rounds 6-11 archived verbatim (1016 -> 381 lines); CHANGELOG "local" lines now name the machine (Linux e2b.local 6.1.158+ x86_64, gcc 12.2.0); owner quotes in docs English.

**Verification (local sandbox 2026-10-30):** make test-run -> hydra 234/0 (baseline 234 unchanged), jni 128/0, large 7/0, hydra2 152/0, **hydra-moe 180/0** (138 -> 180), gguf 101/0, converter 38/0; text_prompt_check ok; server_test.js 47/0; ASan+UBSan **detect_leaks=1** all five binaries 0 errors/0 leaks; gcc -fanalyzer rc 0 (hydra_moe.c notes 40 -> 26, pre-existing false positives); workflow gates ok.

**Status:** all gates local green. Next: commit + push + CI + PR body update (FACTS ONLY).

**Status:** all gates local green. Next: commit + push + CI + PR body update (FACTS ONLY).

## Round 14 (2026-10-31) — merge follow-up: macOS sanitizer crash, fix, re-green

**Session start (R33):** 2026-10-31, branch `arena/44edb923-bestai` (PR #73 already merged as `d1729ba`), base = origin/main. Read: `.agents/errors.md` (macOS/AppleClang entries), `.github/workflows/ci.yml` (sanitizer step), PR #73 merge run.

**Owner directive (verbatim):** "Please merge the pull request." / after the red main run: "fixen und so!!" (fix it and the usual follow-up).

**What happened:**
- PR #73 merged (merge commit `d1729ba`, merge-commit style per the repo convention of PR #70).
- The main-tier CI run of the merge FAILED exactly one step: `build-test (macos-latest)` → "Sanitizers (ASan+UBSan) over the whole test suite", exit 134 (SIGABRT). All other jobs/steps passed, including the ubuntu leg of the same step and the full Release workflow.
- Root cause: round 13 (Qodo #6) set `ASAN_OPTIONS=detect_leaks=1` unconditionally. AppleClang ships NO LeakSanitizer; with detect_leaks requested the ASan runtime aborts at startup on macOS. The ubuntu-green leg was no proof for the macOS leg - a platform-specific runtime option was changed without a macOS verification path (no macOS toolchain in this sandbox).
- Fix (commit `fe2edc6`, PR #75): platform-conditional option in the sanitizer step - `detect_leaks=1` on Linux, empty on macOS; ASan + UBSan (`halt_on_error=1`) stay fully active on both legs. workflow_yaml_check + workflow_lint_check green.
- PR #75 merged (`5ff3011`). Main-tier run: all three workflows SUCCESS, `build-test (macos-latest)` success (9/9 jobs). Main green.

**Lesson (recorded in `.agents/errors.md`):** sanitizer/ASan-RT options are platform-specific; a change to them must be verified on EVERY matrix leg (or guarded by `uname -s`) before it can run on one. Exit 134 = SIGABRT at runtime startup pointed at a runtime-option problem, not a test failure.

**Workspace note (2026-10-31):** after the merge the sandbox workspace was re-created as a fresh clone (branch pointer at the session base `94f8e08`); 7 files newly added in round 12 were missing from the restored working tree (present in the commits - nothing lost). Working tree restored via `git reset --hard 3c448c9` before the follow-up fix.

## Round 15 (2026-10-31, R33 marker below) — large-model analysis (700B→8B), 2026 technique scan, Mermaid architecture in README, Q4_0/Q4_1 GGUF import

**Session start (R33):** 2026-10-31, branch `arena/8467cfad-bestai` (base = origin/main `40a4f69`, PR #73/#75/#76 all merged, main green). Read before touching code: `.agents/README.md` -> `session.md` (round 14 + protocol) -> `rules.md` (R1-R33) -> `user.md` (owner directives) -> `Environment.md` -> `errors.md` (top entries) -> `README.md` (current claims) -> `docs/GGUF-IMPORT.md` §12 + `tools/gguf_reader.py` (decoder infrastructure) -> `tools/gguf_minimal_llama.py` (fixture writer). Web: 2026 sources listed in `docs/LARGE-MODELS.md` §7 (NVFP4/MXFP4 in llama.cpp, GPT-OSS-120B, on-device RAM/tok/s tables, 405B server numbers, KV-quant/speculative/prefix-cache stack, Android NPU 2026).

**Owner directive (verbatim intent):** look at the web + docs how everything works; create an intelligent final Mermaid file that GitHub renders, put it in the main README; analyse how a 700B model converts into our format with no lag and no intelligence loss and runs everywhere incl. Android (or are we limited to ~200M); search 2026-current techniques that can be integrated (new files allowed); then docs; then improve the tests and run them locally so 30B/8B models work (adapting mainly toward 8B for now); adapt the Android app if changes are needed.

**What was built (all local gates green):**

1. **Q4_0 + Q4_1 GGUF import** (the 8B-class quants, 2026's workhorse 4-bit block-scaled family, same lineage as the MXFP4/NVFP4 that landed in llama.cpp in March-April 2026):
   - `tools/gguf_reader.py`: `BLOCK_TYPES += {Q4_0: (32,18), Q4_1: (32,20)}`; decoders `_decode_q4_0` (`w = d*(q-8)`) and `_decode_q4_1` (`w = d*(q-8)+m`, exactly ggml's `dequantize_row_q4_1` expression), all C-speed (strided slice gather + `bytes.translate` tables + `array` map/mul - no Python value loop, so an 8B model's 4e9 values do not pass through Python).
   - `tools/gguf_minimal_llama.py`: fixture emits `--dtype q40/q41` (deterministic quantisation: F16-exact scales 0.1/0.05, codes clipped to the 4-bit range, low-nibble-first packing per ggml-quants.c).
   - `tools/gguf_to_hydra2.py` + both converter message/docstrings: Q4_0/Q4_1 decode in place to F32 - an 8B Q4_0 GGUF (4.9 GB) converts without a `--convert-f16` re-export; the `.hydra2` stays F32-resident (the v2 container's own `Q4_0` type `0x0030` remains reserved = the 4-bit-resident engine milestone, documented).
   - Gates: converter 35 -> **44 checks** (Q4_0/Q4_1 fixtures convert + run + deterministic, Q4_0 stream differs from the F32 stream = quantisation visible, embed F32 **bit-exact against an independent from-scratch decode** in the test); `gguf_test.py` 101/0.
2. **`docs/LARGE-MODELS.md` (new)**: the honest 700B answer (350 GB at 4-bit - no device path exists, 2-bit still 185 GB; "no loss" is the F32 reference, a precision ladder is the product answer; "no lag" is bandwidth), the memory table (8B→700B x F32/16/8/4/2-bit), the 2026 technique scan with verdicts (MXFP4/NVFP4 -> adopted at the converter boundary now, T4_0 engine type = next engine milestone; KV INT8/FP8 = round-16 candidate, needs re-pinning; speculative decoding = best fit of all, v1 ternary as draft for the v2 target; prefix caching = cheap server win; paged attention = deferred, not needed single-stream; QAT = the honest answer to zero-loss at 4-bit; NPU QNN/LiteRT-LM = the phone "no lag" answer, NEON stays the universal path), the concrete 8B path (what works today, the 4-step phone milestone list, the 12 GB phone memory budget), the 30B/70B/120B-MoE/700B table (120B MoE is the showcase: expert streaming is its access pattern), the hybrid architecture (8B local + big server via `server.js`), the Android section (nothing breaks this round; the app already runs 8B GGUFs through the bundled llama.cpp; the v2 engine joins the .so after the NEON parity milestone), and the sources.
3. **README**: new `## Architecture` section with the system-wide Mermaid flowchart (GitHub renders it natively: model sources -> converter -> .hydra2 -> MoE/v1 engines -> CLI/web/Android -> the gate wall), an honest v1-vs-v2 paragraph, pointers to the four spec docs; converter bullet now states the six decoded layouts + the 44-check gate.
4. **Docs sync**: `docs/GGUF-IMPORT.md` (reader table + Q4 layout descriptions + §9 + §12 + gate paragraph), `docs/FORMAT-V2.md` (the decoder paragraph + the import-path sentence; the v2 container type table is unchanged - `0x0030` Q4_0 stays reserved, that is the honest state), `tools/gguf_to_hydra.py` refusal messages now name all six layouts (the shared reader made them true).

**Verification (local sandbox 2026-10-31: Linux e2b.local 6.1.158+ x86_64, gcc 12.2.0, Python 3.11.2):** hydra 234/0 (baseline `x86` 234, count gate passes), jni 128/0, large 7/0, hydra2 152/0, hydra-moe 180/0, gguf_test 101/0, **converter 44/0**, text_prompt_check ok, server_test 47/0, workflow_yaml + workflow_lint ok, docs_claim_check ok, pyflakes rc 0, bandit rc 0. C sources unchanged this round (Python + docs only), so the ASan/fanalyzer states of round 14 carry over unchanged.

**Status:** all gates green; committed and pushed, **PR #77** opened (FACTS ONLY). PR-tier CI: `CI` 4/4 runnable jobs success (build-test/neon-test/Android APK/GPU bench skip by design - main tier), `Lint & Security` 11/11 success. Awaiting the owner's merge signal (main-tier jobs run after the merge).
