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


## Round 16 (2026-10-31) — 3B-Ready + T4_0 4-bit resident engine (PR #77, `aeb978e`)

*(Section written retroactively during the round-17 documentation audit —
the round's own plan log was never saved. Every fact below was re-verified
against the merge commit, CHANGELOG.md and the live E2E gate during that
audit.)*

**Owner directive (verbatim):** "Bereite uns 3B + die 4bit Engine vor"

- **A1 — T4_0 4-bit resident engine type (the main deliverable).**
  `HYDRA2_Q4_0` (0x0030) graduated from reserved to implemented: the
  container validates the 18 B/32 block layout and the /32 element group;
  `expert_layout` 2 = Q4_0 expert blob ([gate][up][down] as 18-byte
  blocks); the mat-vec kernels dequantize ONE row at a time into a shared
  scratch — resident memory stays 4-bit, F32 is never materialized (that
  is the point of the type). Block = F16 `d` + 16 nibble bytes,
  `w = d*(q-8)` (FORMAT-V2 §3.2). Parity vs the F32 fixture within test
  eps (40 checks in tests/test_hydra_moe.c).
- **A2 — streaming Q4_0 converter path.** `tools/gguf_to_hydra2.py`:
  `--t4` (all trunk tensors written as Q4_0 blocks; Q4_0 SOURCE tensors
  are copied bit-exact; dim/inter % 32 enforced) and `--probe` (reports
  whether a model is Q4_0-clean). Still 32-chunk streaming. The reference
  quantizer is stdlib-only; an optional numpy fast path (used only when
  numpy is importable) produces bit-identical bytes.
  `gguf_to_hydra2_test.py`: 44 checks.
- **B1 — server configuration surface.** Env: `HYDRA_DEFAULT_MODEL`,
  `HYDRA_MAX_TRAIN_BODY`, `HYDRA_MAX_PROMPT` (only a CAP, default 256),
  `HYDRA_INFER_TIMEOUT_MS`, `HYDRA_DEFAULT_STEPS`, `HYDRA_IMPORT_MODE` +
  `GET /api/config` (documented, read-only). Import:
  `?mode=auto|v1|v2&t4=auto|force|none&max_experts=1..8`; v2 target →
  `models/converted/<name>.hydra2`. `server_test.js`: 57/57.
- **B2 — 3B synthetic fixture.** `tools/build_3b_synth_gguf.py`: 3072 MB
  GGUF, 11,696-tensor listing, llama architecture, vocab 128256, 36
  layers, 32/8 GQA heads, inter 7680, rope 5e6, BOS 128005, Q4_0 trunk
  (10,153 entries) + F32 attention/routing, tied embed, 16 B merges;
  md5 `51943c49967b`.
- **E2E (opt-in, `HYDRA2_3B_E2E=1`, run on a 64 GB host):** the server
  imports the 3B with `--t4` → `models/converted/3B-geometry-synth-v2.hydra2`
  (1569 MB); the container loads; the MoE forward runs 16 tokens
  (`64241 360 360 360 …`); md5 `6848f37ecd55c2777674d97aea1865bf`; peak
  2.58 GiB RSS (vs 10.7 GiB F32); 5.98 s/token; fixed point 77348.
- **Golden pins:** mini-f32 `4e3fc239bd63`, mini-q40 `da9a59aca458`,
  t4src-f32 `c9ce7a1074d2`.
- **Decisions:** no Android changes; no release (PR-tier CI verifies
  functionality, release stays on the main tier); T4_0 is Q4_0-accurate,
  not lossless — it is the Q4_0 block, documented in FORMAT-V2 §3.2.
- **Gates at merge time:** `make test-run` 234/128/7/101/152/216;
  `server_test.js` 57/57; converter 44; PR-tier CI 16/16.
- PR #77 `aeb978e` (commits `208c1d5` server config, `ce906a4` engine +
  converter + 3B, `297c4b5` docs) — merged 2026-10-31; the first main-tier
  run afterwards went RED (next section).

## Round 16.5 (2026-10-31) — main CI red after #77: fixture-list drift (PR #78, merged as `8606d58`)

- **Incident:** after the #77 merge, main-tier `build-test` (macos +
  ubuntu) failed in the Sanitizers step (run 41222651267). Root cause:
  the ci.yml sanitizer step carried an INLINE copy of the hydra-moe-test
  fixture list from before R16 added `-q4`/`-q4f32`; the binary now
  hard-requires 9 fixture argv entries (argv-count check before any test)
  → loud refusal. `make test-run` was green (the Makefile had been
  updated in #77) and the PR tier is lint-only by owner rule, so the
  drift was invisible until the first main-tier functional run.
- **Fix (PR #78, `c5d281e`):** `tools/make_moe_fixtures.sh <base>` is now
  the SINGLE source — it generates all nine fixtures
  (`<base>`, `-t64`, `-t128`, `-t64-lg`, `-nv`, `-at64`, `-gqa`, `-q4`,
  `-q4f32`) and prints their paths as the last stdout line, in the argv
  order `tests/test_hydra_moe.c` expects. The Makefile `test-run` recipe
  and the ci.yml sanitizer step both call the script — no copy left.
  The 9-argv contract in the test binary is unchanged.
- **Proof:** the 7-argv error was reproduced locally BEFORE the fix;
  after the fix `make test-run` 234/128/7/101/152/216 all 0, and the
  sanitizer leg reproduced byte-for-byte locally (ASan + UBSan,
  `ASAN_OPTIONS=detect_leaks=1`, `UBSAN_OPTIONS=halt_on_error=1`) →
  exit 0.
- **Merged** 2026-10-31 17:43Z → `8606d58`. Main CI run 41225936406:
  9/9 jobs success (build-test 30s = normal — the red run needed 36–38s
  to reach the same step); Lint 41225936262 success; Release
  41225936402 success. Main is fully green.
- **Tooling note:** `gh run view … --log` returned EOF repeatedly from
  the results-receiver; the proof came from job conclusions + step
  timestamps + local reproduction (logged in .agents/Environment.md).
- Full incident + prevention: `.agents/errors.md` (last entry, 2026-10-31).

## Round 17 (2026-10-31) — full documentation audit + README in every folder + Linear sync

**Session start (R33):** 2026-10-31, branch `arena/1f80aaf1-bestai`
(base `8606d58` = origin/main, clean). Read first: `.agents/session.md`,
`.agents/errors.md`, `.agents/rules.md`, `.agents/user.md`, then the
`.agents/skills/repo-docs-audit/SKILL.md` checklist. **No code or doc
edit before this marker.**

**Owner directive (verbatim):** "Ok lese dir die ganze DOKU durch, und
bitte alles aktualisieren, auch wenn du was auf Linear gepostet hast und
so, bitte alles aktualisieren, finde in jedem ordner ne Readme und prüfe
diese gegen den Code bitte!"

**Scope:** (a) every .md in the repo read, each claim verified against
the code (R6/R7 proof obligation), stale facts updated; (b) a README in
EVERY folder (create the missing ones); (c) the Linear project brought
up to date (R16 + CI fix + this audit); (d) the repo-docs-audit skill:
4 mechanical checks green + per-file findings below.

**Per-file findings (one line per .md — the skill's required list):**
- README.md — FIXED: mermaid suite counts moe 180→216, server 47→57;
  roadmap moe count 180→216; tools tree +make_moe_fixtures.sh,
  +build_3b_synth_gguf.py. R16 content (T4_0, --t4, --probe, 3B E2E) was
  already present and verified against code.
- CHANGELOG.md — FIXED: two new top entries (CI fix PR #78 / R16 PR #77).
- AGENTS.md — FIXED: §2.3 six GGUF layouts + gguf_to_hydra2.py +
  make_moe_fixtures.sh; §3 counts jni-batch 128, server 57, +gguf_to_
  hydra2_test 44; §5 FORMAT-V2 row = implemented (not "spec only");
  §2.4 +/api/config + import query surface; §2 map +v2 container paths.
- CLAUDE.md (root) — OK: import-only bridge, no content of its own.
- docs/ARCHITECTURE.md — FIXED: Components table +v2 container / MoE
  engine / v2 converter / server rows; "no SIMD kernel for the v3/v4
  path yet" → tf_row_dot has AVX2 + NEON since R11 (bit-identical,
  parity-pinned); R16 state added.
- docs/FORMAT.md — FIXED: top note (the v2 container HAS a loader +
  engine now), layout version rule (1..4), rule 7 +v4 sizes.
- docs/FORMAT-V2.md — FIXED: status banner (implemented since R12);
  §3.1 type table Q4_0 = implemented/resident (R16); §3.2.2 +expert
  layout 2 (Q4_0); §4.4 +v2.layers row, expert_layout "0/1/2",
  "All ten"→"All thirteen"; §6.3 Q4_0 no longer reserved; §7.3
  180→216; §7 implementation status +R16.
- docs/GGUF-IMPORT.md — FIXED: §12 numpy wording (optional fast path,
  bit-identical; the core is stdlib-only); §12.3 Q4_0 row verified
  against code (type 0x0030, 18 B/32).
- docs/LARGE-MODELS.md — FIXED: §3 "next engine milestone" → done in
  R16 (pointer §8.1); §4 milestone 1 marked done, milestone 2 reworded
  (geometry is metadata-driven, engine default 4+128, converter writes
  2+8, paged KV not implemented). §8 (R16 record) verified accurate.
- android/README.md — OK: no Android change in R15–R17; ABI/import-limit
  claims re-verified against MainActivity.kt / CMakeLists.txt.
- tools/gpu_bench/README.md — OK: NOT-MEASURED status still true.
- tools/kotlin_stub_check/README.md — OK: all claims current.
- tools/README.md — NEW (was missing): tool inventory, generated from
  the tools/ directory + Makefile/CI usage.
- src/README.md — NEW (was missing).
- include/README.md — NEW (was missing).
- tests/README.md — NEW (was missing).
- public/README.md — NEW (was missing).
- docs/README.md — NEW (was missing).
- .agents/README.md — FIXED: SESSION-ARCHIVE description (now rounds
  3–15); docs table +ISSUE-72-AUDIT.md, +LARGE-MODELS.md.
- .agents/CLAUDE.md — OK: bridge + one-line rule pointers, no divergence.
- .agents/rules.md — OK: R1–R33, nothing stale.
- .agents/session.md — FIXED: R16 + R16.5 sections were missing (added,
  R16 marked retroactive); R12–R15 archived verbatim to
  docs/SESSION-ARCHIVE.md; this R17 section.
- .agents/errors.md — OK: the R16.5 fixture-drift entry is already the
  last entry.
- .agents/user.md — FIXED: +item 31 (this directive, verbatim).
- .agents/Environment.md — FIXED: +2026-10-31 entry (fresh /tmp/lintenv,
  npm ci, CI log EOF + check-runs steps null + jobs-API timestamps).
- .agents/docs/ANDROID-AUDIT.md — OK: dated point-in-time audit
  (2026-10-26); findings describe the audited state, kept as a record.
- .agents/docs/ANDROID_SKILL.md — OK: no stale counts (checked the
  43/57/128 candidates; none present).
- .agents/docs/HYDRA2-RESEARCH.md — FIXED: status banner note (the
  engine this spec motivated is implemented, R12–R16; pointers).
- .agents/docs/ISSUE-72-AUDIT.md — OK: audit record; the "no .hydra2
  loading via JNI" claim re-verified TRUE (JNI has no v2 include).
- .agents/docs/RESEARCH.md — OK: BitNet/W1.58A8 facts still accurate.
- .agents/docs/RESEARCH-LOG.md — FIXED: index missing rows for §9/§10
  (added); duplicated closing paragraph removed (copy-paste artifact);
  +§11 (2026-10-31, the fixture-drift investigation).
- .agents/docs/SESSION-ARCHIVE.md — FIXED: +R12, R12 progress, R13,
  R14, R15 appended verbatim (2828→3199 lines).
- .agents/docs/TECHNICAL-AUDIT.md — OK: dated point-in-time audit;
  its quoted "no v2 loader" line is the audited state, kept.
- .agents/docs/gpu-feasibility.md — OK: NOT-MEASURED discipline intact.
- .agents/docs/pr-gpu-feasibility.md — OK: PR text record.
- .agents/skills/engine-ci-verify/SKILL.md — FIXED: the gate list was
  pre-R12 (no moe/gguf/server suites, old sanitizer shape); updated to
  the current CI (make test-run, the fixture script, the extra jobs).
- .agents/skills/{android-lint-gate,android-ndk-build,ci-triage,
  kotlin-stub-check,repo-docs-audit,session-workflow}/SKILL.md — OK:
  frontmatter valid, facts verified against code/workflows.
- include/hydra_moe.h — FIXED (code comment = docs, R31): header block
  lists only layouts 0/1 and "the contract of this round" (stale since
  R16); the GQA comment says `i % n_kv_heads` but the code (and the
  R13 fix) use the block mapping `i / (n_heads / n_kv_heads)`.

**Fix list (all verified against code before writing):** [as in the
findings above]

**Gates (2026-10-31, after the last edit):**
- `gcc -fsyntax-only` strict (CI 1a) rc=0; clang strict: BLOCKED in this
  sandbox (no clang, apt unreachable — measured), CI covers it.
- cppcheck 2.22 (`/tmp/lintenv`, rebuilt this session): 9/9 files, 0
  warnings, rc=0.
- `make clean && make && make test` + `make test-run`: 234/128/7/101/152/
  216, every suite 0 failures, rc=0.
- `tools/test_count_check.sh ./hydra-test x86`: 234 = baseline.
- `tools/ci_syntax_mirror_test.sh` rc=0 (local/CI syntax parity).
- Python: `py_compile`, `pyflakes tools/` (exact CI command), `bandit -r
  tools/ --severity-level medium` (severity 0/0) — all rc=0.
- Node: `server_test.js` 57/57, `console_train_test.js` pass,
  `contrast_check.js` 17 pairs, `npm run lint` (ESLint) clean.
- Sanitizers VERBATIM CI shape (5 ASan+UBSan binaries, `detect_leaks=1`,
  `halt_on_error=1`, pyfix via `make_model.py --v2`, moe fixtures via
  `tools/make_moe_fixtures.sh`): all five exit 0, no diagnostics.
- `make_model.py` starter `cmp` vs `android/.../starter.hydra`: identical.
- `text_prompt_check.sh`: ok.
- Shell guardrails: docs_claim (the README moe-216 claim now passes),
  workflow_yaml (5 files), workflow_lint (4 files), session_start (R33
  marker present + negative control), sampler_contract 34,
  android_autostart 32 + negative 15, android_header 28, android_crash 25,
  compose_scroll 7, string_format 37, ci_release_version (all cases),
  llama_pin 5/5 + negative 7 — all green. `jni_signature_check.sh`: SKIP
  (no javac in this sandbox — reported per the skill; CI has JDK 17).
- BLOCKED in this sandbox (recorded in Environment.md, CI covers):
  clang/clang-tidy/`clang --analyze`/flawfinder (apt unreachable),
  aarch64 NEON + qemu (no cross toolchain), javac, Android gradle build.
- `gcc -fanalyzer` + C++ parse: rc=0.

**Linear (synced this session):** verified empty first (title search
"hydra"/"bestai" returned 0 issues — same as the round-5 record). On
owner request: project **Hydra-Stone (BestAI)** created
(`97af577a-8b90-4717-8128-965f36c5cf89`, team PRO) with 3 tasks:
- **PRO-59** — Round 16 engine + docs (Done; PR #77 attached, merged
  `aeb978e`, CI 41225936406 9/9).
- **PRO-60** — CI incident 41222651267, moe fixture drift (Done; PR #78
  attached, merged `8606d58`).
- **PRO-61** — this round 17 audit (In Progress until merge).
