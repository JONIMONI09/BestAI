# Changelog

All notable changes to this project, per change, are recorded here and in
`session.md` (rules.md R2). Newest entries first.

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
