# Changelog

All notable changes to this project, per change, are recorded here and in
`session.md` (rules.md R2). Newest entries first.

## 2026-10-25 — M2: Min-P sampling + repetition penalty (greedy guard keeps the baseline)

Adds controlled randomness to the deterministic decoder without touching
the greedy path:

- **`include/hydra_model.h`**: new `hydra_sampler_config_t`
  (`temperature`, `min_p`, `repetition_penalty`, `penalty_window`,
  `rng_seed`) and `hydra_sample_token()`. `HydraEngine` gains one
  `uint64_t rng_state` field (zero after load; never read by greedy
  decode), so a full run is reproducible from (model, start token,
  config).
- **`src/hydra_engine.c`**: `hydra_sample_token()` implements the
  documented two-mode contract:
  - **Greedy guard** — `temperature <= 0` OR `min_p >= 1` delegates to
    `hydra_engine_step()`: bit-identical output, which is what keeps
    the deterministic baseline sequence valid.
  - **Sampling** — the engine has no native per-vocabulary logits, so
    the candidate logit is the engine's own quantity:
    `L(c) = agg_a[0]*c + agg_b[0]*state[0]` (the acc[0] that would be
    computed if c were fed). Then: repetition penalty over the recent
    window (positive logits divided, negative multiplied, repeats
    compound) → numerically stable softmax (max subtracted before
    exp) → min-p filter (drop `P(c) < min_p * p_max`, renormalize) →
    seeded xorshift64* CDF walk → state advance through the real
    `hydra_engine_step(chosen)`, so the state machine is the SAME as
    greedy mode. Zero heap (one 8 KiB stack array for the vocab cap).
- **`src/main.c`**: new flags `--temp`, `--min-p`, `--rep-penalty`,
  `--rep-window`, `--seed` (defaults keep the CLI bit-identical:
  temp 0 → greedy). `--fast` is ignored with a visible note when
  sampling is active (sampling advances the full state by contract).
  The console output shows the active sampler config.
- **`tests/test_engine.c`**: 32 new checks (all architectures), all
  expected values hand-computed against an independent Python
  reference (double softmax + identical xorshift64* stream), not
  fitted:
  - greedy guard (temp=0 and min_p=1) bit-identical to the native
    step over 32 steps,
  - repetition penalty breaks the repetition loop on a positive logit
    (argmax 63→62 at pen 4, T=0.001 where the softmax collapses
    exactly onto the argmax) AND multiplies a negative logit
    (model with state 14: argmax 7→6, outputs 1→7),
  - min-p surviving sets verified over fixed seeds: min_p=0.5 →
    always the argmax token (16 seeds), min_p=0.1 → only tokens
    {62,63,64} (24 seeds, ≥2 distinct), and the control without the
    filter reaches the tail (96 seeds),
  - exact xorshift64* draws for seeds 1/2/555/0 pinned to the
    hand-computed CDF bins (the seed-0→magic-state degeneracy guard
    included),
  - full 64-step seeded reproducibility,
  - argument negative controls (NULLs, vocab<2 → −3, temp>20 → −4,
    penalty<1 → −1).
- **`tools/test_count_baseline.txt`**: x86 81→113, arm 87→119 (the 32
  new checks compile on every architecture; ARM re-measured on the CI
  leg).

Measured: `./hydra-test` 113/0 (x86); baseline run
`[471, 386, 385, 386, 385, 386, 385, 386]` @ start_token=42 unchanged;
`jni-batch-test` 86/0, `large-model-test` 7/0, `gguf_test.py` 40/0.

## 2026-10-25 — M1: agent hygiene (CLAUDE.md bridge, dead artifact removal)

- **`CLAUDE.md`** (new): one-line bridge that loads `@AGENTS.md` first
  and restates the always-on rules, so Claude Code sessions and the
  repository's agent system can never drift apart.
- **`hydra_stone_model_engine.zip`** removed from git (`git rm`):
  grep over the whole tree shows it is referenced nowhere except
  session-history text — an old build artifact that bloated every
  clone. (Resolves the open decision from the 2026-10-25 audit.)
- Verified (no change needed): `.gitignore` already excludes
  `.agent_memory/` and `RESEARCH.md`, so the local research mirror and
  Notion sync notes stay out of the repository.
- The audit + derivations for this round are recorded in
  `.agent_memory/RESEARCH.md` (local fallback mirror; Notion "AI
  Notizen" sync follows at round close).
- **Not done (deliberate):** the spec title mentions "AGPL-3.0", but
  the LICENSE file is GPL-3.0 and the spec body never asks for a
  license change — changing it is a legal decision for the user, not
  an implementation task. The LICENSE file is untouched.

## 2026-10-25 — post-merge doc audit (screenshot verification pass)

PR #48 merged (20:04Z, main = afae8c8). Audit of the GitHub README
state + final docs across all folders, per user request.

### Fixed

- **License contradiction.** The LICENSE file is the full GPL-3.0
  text (GitHub's own header detects "GPL-3.0 license"), but README.md
  said "MIT — see LICENSE" in two places (License section and the
  project-structure tree) and package.json carried
  `"license": "MIT"`. All three references now say GPL-3.0. The
  LICENSE file itself is untouched — changing it is a legal decision
  for the user, not a documentation fix. (The MIT references in
  AGENTS.md / docs/ANDROID-AUDIT.md describe the pinned llama.cpp
  v0.5.0, which really is MIT, and stay as-is.)
- **Broken cross-reference in the roadmap.** The `.hydra2` item
  pointed readers to a "*Roadmap status* section" of
  `docs/ARCHITECTURE.md` — a section that never existed in that file
  (`git log -S` shows it was only ever written in the README). The
  superseded aggregated `A[i]/B[i]` design is documented under
  "Known Structural Redundancy: the Layer Loop Is Algebraically
  Free"; the reference now names that section.

### Verified (measured or checked against the tree — no change)

- Token sequence `[471, 386, 385, 386, 385, 386, 385, 386]` for the
  starter model (start_token=42, 8 steps) — reproduced with a real
  build + run.
- CI enforces the ARM64 half on every push: the `neon-test` job
  cross-compiles for aarch64, runs the suite under qemu (scalar vs
  NEON comparison happens inside the same ARM binary,
  `tests/test_engine.c` under `#ifdef __ARM_NEON`), and replays the
  parity run `42 8 --json`.
- The emulator evidence claimed for Android is present in
  `android/README.md` ("host and emulator both produced
  `[471, 386, 385, 386, …]` — bit-identical").
- `tools/make_model.py` guarantees AND asserts `A[0] != 0` and
  `B[0] != 0` (the state-blind `B[0] == 0` history is documented in
  the file).
- Web console: zero runtime npm dependencies, ESLint dev-only and
  used by the lint gate; `make ui` builds hydra-run + the starter
  model and serves on port 8787 (`server.js`).
- "4-weights-per-byte packing" roadmap item: `docs/FORMAT.md`
  "Version 2" section and `tests/test_engine.c`
  `test_version_two_format` both exist.
- ".hydra2 not implemented": no `hydra2` anywhere in `src/` or
  `include/`; `docs/FORMAT-V2.md` is the spec.
- `tools/docs_claim_check.sh`: all banned claims absent.

## 2026-10-25 — post-merge lint fix (branch `arena/6bf4fc8e-bestai`)

### Fixed

- **Android Lint findings broke the Lint & Security and Release workflows
  after the #47 merge** ("Android Lint reported findings" / "Android
  Lint must be error-free"; the gate greps the report for "No issues
  found", so even warnings fail it). Root cause: the three M5 telemetry
  strings (`run_live`, `run_done`, `run_done_cancelled`) were plain
  `<string>` resources containing `%1$d tokens` — a quantity — which is
  exactly what the `PluralsCandidate` check flags, and it violated the
  repo's own established convention (`log_size`, `log_running`,
  `log_bench_start` are all `<plurals>`; no non-plural `%d` string
  existed before this merge, which is why the gate had been green).
  Converted all three to `<plurals>` (one/other, `%1$d` positional,
  matching the existing entries) and updated the three `stringResource`
  call sites in `HydraApp.kt` to the plurals form
  (`quantity, args…`). CI compile + APK build + APK verification were
  already green on the merged commit — only the lint gate was red.
  **Round 2 (after the first CI run):** the plurals conversion removed
  all warnings (report: "3 errors, 0 warnings"), but the
  `stringResource(R.plurals…, …)` call style hit a second wall: in this
  Compose version (BOM 2024.09.03) overload resolution picks the
  `stringResource(@StringRes Int, vararg Any)` overload, so lint raised
  three ERROR-level `ResourceType` findings ("Expected resource of type
  string"). The call sites now use
  `LocalResources.current.getQuantityString(R.plurals…, …)` — one
  unambiguous signature, a `@PluralsRes` parameter, and the same pattern
  `MainActivity` already uses for its plurals. The strings stay plurals.
- **`.github/workflows/lint.yml` + `release.yml` — self-publishing lint
  reports.** The report was only readable in the Actions UI (job log +
  artifact, both on hosts outside API-only egress). Two changes make
  failures diagnosable from the API alone: (1) the `gradle lintDebug`
  step no longer aborts the job (it captures the non-zero exit — gradle
  writes the report before it aborts anyway); (2) the dedicated check
  step now publishes the full report on failure as a PR comment (via the
  issues API; the PR number comes from the event, or is resolved from
  the commit SHA via `/commits/{sha}/pulls` so push runs that update a
  PR are covered too) and to the job step summary (always), and fails
  the job itself. The first attempt (199221c) proved why the
  restructuring was needed: the publishing step sat behind the gradle
  step and was skipped on exactly the runs where the report was needed.
  Round 3 (b53d738) confirmed the restructure works (gradle step
  non-fatal, check step ran and failed the job as the sole gate), but
  the PR comment still did not arrive: the workflow token has only
  `contents: read`, so the issues API call 403s and the defensive
  `|| true` hid it. Round 4 adds job-level
  `permissions: {contents: read, issues: write}` on the android-lint
  job (lint.yml) and the build-android job (release.yml) — scoped to
  exactly the jobs that need it — plus a visible `::warning::` marker
  on the comment fallback path so a broken publish cannot hide silently
  again. Round 5 closes the last diagnostic gap: "report missing" is
  a second failure class (gradle dying before the lint analysis — e.g.
  a Kotlin compile error — produces no report file at all). The gradle
  steps in lint.yml and ci.yml now capture their full output to a
  file, and the check/publish step builds its payload from whichever
  class occurred — the lint report when it exists, the tail of the
  gradle log (150–200 lines) when it does not — published as step
  summary + PR comment in both cases. The CI "Android APK build" job
  got the same capture-and-publish treatment (plus job-level
  issues:write). From the next failed run onward, every Android
  failure in CI, Lint & Security, and Release is readable via the API.
  the lint failure artifact now also bundles the diagnostic payload
  and the gradle log. **Round 6 — root cause found and fixed:** all
  the red runs since 199221c (CI build step twice, Android lint "no
  report") were one single Kotlin compile error: the round-2 fix used
  `androidx.compose.ui.platform.LocalResources`, which does not exist
  in this Compose version (BOM 2024.09.03) — an unverified API
  assumption, used nowhere else in the repo. Call sites now use
  `LocalContext.current.resources` (LocalContext is already used in
  this codebase, e.g. Theme.kt); everything else (the plurals
  resources, the getQuantityString call style) is unchanged.

## 2026-10-25 — CI fix round (post-PR, same run)

### Fixed

- **`android/.../MainActivity.kt` — `selectEngine()` nullable receiver (CI
  compile error, `MainActivity.kt:987:57`).** The M5 edit passed
  `file.absolutePath` into `detailFor()` while `file` was still typed
  `File?`; `result.ok == true` implies a file exists, but the compiler
  cannot see through a data-derived boolean. Restructured: the no-model
  case now early-returns (identical UI state and log line as before), and
  everything after the `file == null` check runs on a smart-cast
  non-null `file` — `file.name` / `file.absolutePath` compile, and the
  success/failure UI behaviour is byte-for-byte the same. Not compilable
  in the audit sandbox (no kotlinc/NDK egress) — verified by reading;
  the CI build is the compile proof.
- **Gate scripts missing the executable bit (5 of 12 were 100644):**
  `android_autostart_negative_test.sh`, `android_crash_check.sh`,
  `android_header_check.sh`, `ci_release_version_test.sh`,
  `jni_signature_check.sh` are now 100755 in git. On a checkout whose
  umask is 000 these files appeared as `-rw-rw-rw-` (666) and could not
  be executed directly; every shipped script with a shebang now carries
  the exec bit, so no checkout can produce a non-executable script.
  All 12 `tools/*.sh` files are 100755; JS/Python tools are deliberately
  644 (invoked through `node`/`python3`, never directly).
- **CI `upload-artifact` "No files were found with the provided path:
  dist/"** — no change needed: the APK job creates `dist/` in the same
  step that inspects the built APK; that step was skipped because
  `assembleDebug` failed on the Kotlin error above. The upload runs with
  `if: always()` + `if-no-files-found: error` on purpose, so a missing
  APK is always visible. With the compile fixed, `dist/` exists again.

## 2026-10-25 — full-repository audit run (branch `arena/4a16d961-bestai`)

Six-milestone run: inventory, docs, conversion math, C/JNI hardening audit,
Android UI, docs/README truthfulness. Findings registry: `RESEARCH.md`
(git-ignored, local only) F-1 .. F-16.

### Fixed

- **`src/hydra_engine.c` — version-2 loader guards (F-1).** Header version 2
  (4 weights/byte) was accepted but had two real gaps: (a) `dim % 4 != 0`
  made the aggregation silently zero out the last `dim % 4` dimensions —
  now refused with error `-8` before any byte is read; (b) the
  `weights_len` bounds check applied the v1 size to v2 files, which would
  reject a minimal-but-valid v2 model — the check is now version-aware
  (v1: `layers × dim` bytes, v2: `layers × dim / 2` bytes), short files
  still refused with `-10`.
- **`tools/gguf_to_hydra.py` — `pick_vocab` report fix (F-3).** The vocab
  override path dropped the resolved vocabulary *count* from the
  conversion report (reported the tensor shape instead of the count the
  engine would use). Fixed; two regression checks added to
  `tools/gguf_test.py` (a dedicated override output path so the main
  conversion file used by later checks is never clobbered).
- **`tools/hydra_train.js` — pair search implemented (F-4).** The trainer's
  docstring promised a 3-rung local search (bit flip → pair swap → full
  row) but only the first rung existed, which measurably locked the search
  on some corpora (A/B in RESEARCH.md §5: official corpus 18.2% → 9.1%
  token error, alt3 27.3% → 36.4% with the old code fully locked for the
  whole run). Implemented as documented; `tools/console_train_test.js`
  still passes (it asserts properties, not a basin number).
- **`android/.../ui/HydraEngine.kt` — cancel state lost (F-14).** The run
  summary was read against a phantom `"tokens"` key (the native JSON emits
  `"emitted"`), and a cancelled run surfaced as an error. Summary is now
  built once, after `runInference`, from the native JSON; `cancelled`
  yields a completed run with a null error; dead `parseSummary` removed.
- **`README.md` + docs — density claims (F-15, F-16).** The on-disk
  density was wrong in five places (README feature line, README table +
  "honest framing" paragraph, `docs/FORMAT.md`, `docs/GGUF-IMPORT.md`
  limits table, `docs/ARCHITECTURE.md`; plus a `server.js` comment).
  Correct: v1 = 4 bits of file per weight (two 2-bit codes per byte,
  upper 4 bits reserved), v2 = 2 bits per weight (4 weights/byte, nothing
  reserved); informational density ≈ 1.58 bits/weight. Table values
  corrected (0.5 B params: v1 250 MiB, v2 125 MiB — was 125/62.5).
  The "~1.06 KiB" engine-context claim was replaced by the measured
  figure: `sizeof(HydraEngine)` = 656 B (576 B step data: two
  `int32[64]` aggregates + one `int8[64]` state). The "planned v2" wording
  became the implemented header-version-2 reference.

### Added

- **`tests/test_engine.c` — `test_version_two_format()` (10 new checks).**
  A v2 file and its byte-equivalent v1 file (hand-decoded fixtures,
  A=[1,1,0,2], B=[-2,-1,-1,-2]) must load, agree on all aggregates, and
  produce identical 40-token streams; v2 with `dim % 4 != 0` must be
  refused with `-8`; v2 with `weights_len < layers×dim/2` with `-10`.
  Falsifiability proved: corrupting one packed bit fails exactly 4 checks.
  x86 baseline 71 → 81 (`tools/test_count_baseline.txt` updated; ARM
  column derived 87 — the ARM CI leg re-measures).
- **`AGENTS.md` (root, canonical).** Architecture map (C/NEON engine,
  GGUF/llama dual engine, Python pipeline, Node/web console, Android
  dual-engine app), `.claude/skills/*` bindings, exact verification
  commands, documentation location table.
- **`docs/FORMAT.md` — "Version 2 (4 weights per byte)" section.** Layout
  (per layer: `dim/4` bytes plane w1 + `dim/4` bytes plane w2), enforced
  constraints with error codes, bounds-check semantics per version, the
  byte-parity test as proof of equivalence, and the explicit
  disambiguation: header version 2 (implemented) ≠ the `.hydra2`
  sparse-MoE container (`docs/FORMAT-V2.md`, spec-only).
- **`docs/GGUF-IMPORT.md` / `docs/FORMAT-V2.md`** — disambiguation notes
  plus the threshold proof section (the converter's `mean|W| × 0.5` cut is
  the reference absmean rule: a weight's code is nonzero exactly when
  `|w| > a/2`; literature cross-check in RESEARCH.md §4).
- **`android/.../ui/HydraApp.kt`** — `RunStats` (live tok/s, latency,
  step count), always-visible `EngineStatusRow` (`.hydra` vs `.gguf`,
  load state) and `RunTelemetryRow` (live / done / cancelled), `ModelRow`
  renders the model detail line. **`android/.../MainActivity.kt`** —
  `detailFor()` publishes llama.cpp's own quant/weights/state figures or
  the hydra format/weights/vocab; failed loads log the cause; stale
  detail is cleared when import-restore fails; live `RunStats` updated in
  `onToken`. **`values/strings.xml`** — +4 English strings.
- **`RESEARCH.md` (git-ignored)** — timestamped full file inventory,
  baseline gate results, findings F-1..F-16, b1.58/ternary-PTQ/edge-
  inference research with sources, live audit of issues #46/#44, trainer
  A/B evidence.
- **Notion page** — "📝 AI Notizen — Hydra-Stone (BestAI) Technical
  Documentation" (under the "AI-TRAINING" parent; local mirror
  `.agent_memory/NOTION_AI_NOTES.md`, git-ignored).
- **`.gitignore`** — `RESEARCH.md` and `.agent_memory/` (local agent
  workspace, never committed).
- **`CHANGELOG.md`** — this file.

### Changed

- **`tools/test_count_baseline.txt`** — x86 71 → 81 (the +10 v2 checks),
  ARM 77 → 87 (derived; CI re-measures). The drift gate fired correctly
  before the update (R21 behaviour confirmed).
- **README roadmap** — "4-weights-per-byte packing" moved to done
  (implemented as header version 2, tested).

### Audited — no change needed (verified, documented in RESEARCH.md §3)

- `src/hydra_jni.c` — fd discipline, local-ref lifecycle, no leaks.
- `src/llama_jni.cpp` — `llama_token_to_piece(…, lstrip=0, special=true)`
  matches the pinned v0.5.0 examples (verified in the submodule source).
- `tools/gpu_bench/` — exemplary "NOT MEASURED" separation + checksum
  gate; no change.
- `src/main.c`, `src/hydra_batch.h`, `src/hydra_neon.h` — clean under the
  M4 audit (leaks, fds, overflow, refs, thread safety).
- Issue #46 — scorecard numbers unverified/stale (F-6); no code action.
- Issue #44 — random-weight "VibeLUTEngine" proposal rejected per R5/R6
  (F-5); no code action.

### Blocked, not measured (stated per R32)

- ARM/NEON parity, APK build, on-device run — no aarch64 cross toolchain,
  qemu, or Android SDK egress in the audit sandbox; CI legs re-measure.
- `tools/jni_signature_check.sh` SKIPs without `javac` (gate itself green).

### Commits

- `1eaef54` — M1 (git-ignored agent workspace; RESEARCH.md)
- `202fef6` — M3 (v2 guards + tests, converter fix, trainer pair search,
  FORMAT docs)
- `c477a52` — M5 (cancel-state fix; engine status + live telemetry UI)
- (M6) — docs/README corrections, CHANGELOG, session close-out
