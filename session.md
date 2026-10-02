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
  second, latent lane-offset bug. 49 tests x86 / 51 ARM.
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

## Current task

**Translate every remaining German `.md` file into English** so the whole
documentation base is in one language.

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
- [x] Verification: `make test` → `=== 49 Tests, 0 failures ===`; final
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