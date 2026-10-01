# Error Log — Hydra-Stone

> Protocol: see `.claude/skills/session-workflow/SKILL.md`.
> Every error gets an entry immediately, with symptom, cause, fix and
> prevention. Newest entries at the top.

---

## 2026-10-01 — Skills rejected by UI: missing YAML frontmatter

- **Symptom:** The Skills UI showed the skill but refused to save/use
  it: *"A skill needs a YAML frontmatter block at the very top, opened
  and closed with a line of three dashes."* All three skills
  (`android-ndk-build`, `engine-ci-verify`, `session-workflow`) were
  plain markdown starting with `# Heading`.
- **Cause:** Skills are metadata-carrying files; the loader parses a
  YAML frontmatter block (`---` … `---`) for `name`/`description`
  before the markdown body. Without it the file is not a valid skill.
- **Fix:** prepend frontmatter to every SKILL.md:
  ```yaml
  ---
  name: <dir-name>
  description: <one sentence, what the skill does + when to use>
  ---
  ```
  Validated all three by parsing the YAML and asserting
  `name == directory name`.
- **Prevention:** Rules R21/R22 — every SKILL.md starts with
  frontmatter; parse and validate it before committing (the check
  script is in the commit message / can be re-run anytime).

## 2026-10-01 — cppcheck: `%u` format with signed int argument

- **Symptom:** `cppcheck --enable=warning` flagged
  `invalidPrintfArgType_uint` in `hydra_engine.c` on the first lint run
  (`%u` receiving `engine->header.version`, a `uint16_t` promoted to
  signed `int` by default promotions).
- **Cause:** default integer promotion makes `uint16_t` an `int` at
  varargs; `%u` then reads an `unsigned int`.
- **Fix:** explicit casts `(unsigned)engine->header.version` and
  `(unsigned)HYDRA_VERSION` in the fprintf call.
- **Prevention:** the lint stage (PR #7) runs cppcheck with
  `--error-exitcode=2` on every commit — caught before merge now.

## 2026-10-01 — Strict C99 without feature macros: POSIX symbols undeclared

- **Symptom:** `gcc -fsyntax-only -std=c99 -Werror` failed with
  `O_CLOEXEC undeclared`, `implicit declaration of madvise`,
  `struct timespec` unknown, `CLOCK_MONOTONIC` undeclared.
- **Cause:** strict `-std=c99` hides POSIX/glibc extensions unless
  feature-test macros are defined.
- **Fix:** compile with `-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE`
  (now part of CI lint gates and `engine-ci-verify` skill).
- **Prevention:** codified in the CI workflow; any new POSIX symbol
  usage is covered by the same gate.

## 2026-10-01 — JNI JSON showed `dim:0, vocab:0, layers:0`

- **Symptom:** on-device logcat: `{"ok":true,"steps":32,"dim":0,…}`.
- **Cause:** `hydra_jni.c` read `engine.header.*` AFTER
  `hydra_engine_unload(&engine)` — which `memset`s the whole struct.
- **Fix:** copy header fields into locals before unload; build JSON
  from those.
- **Prevention:** treat `*_unload`/`*_free` as invalidation points;
  extract all needed data first. Caught only by on-device verification
  — host tests read different fields.

## 2026-10-01 — NEON dispatch bug: nothing accumulated on x86 (caught pre-merge)

- **Symptom:** after wiring NEON chunks into `step()`, the seeded
  variance test failed on x86 (`Varianz: …` FAIL).
- **Cause:** `scalar_start = dim/16*16` was set unconditionally; on x86
  the NEON loop compiles out (`#ifdef __ARM_NEON`), so the scalar loop
  started at `dim` and accumulated nothing.
- **Fix:** `neon_end = 0` when `__ARM_NEON` is not defined (scalar loop
  covers everything); NEON loop runs only when both `__ARM_NEON` and
  `HYDRA_USE_NEON` are defined.
- **Prevention:** the seeded variance test from PR #3 did its job —
  keep falsifiable tests for every code path change.

## 2026-10-01 — CMake: `No SOURCES given to target: hydra`

- **Symptom:** CMake configure failed inside `gradle assembleDebug`.
- **Cause:** wrong relative path depth — `cpp/../../src` resolves to
  `android/app/src/main/src` (nonexistent); repo root is 5 levels up
  from `android/app/src/main/cpp`, not 4.
- **Fix:** `get_filename_component(HYDRA_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)` (five `..`),
  then `${HYDRA_ROOT}/src/hydra_engine.c`.
- **Prevention:** debug the native build directly with the SDK CMake +
  toolchain (see `docs/ANDROID_SKILL.md` §4) before invoking Gradle;
  AGP wraps and hides the real error text.

## 2026-10-01 — NDK clang: implicit declaration of `snprintf`/`clock`

- **Symptom:** NDK build failed with
  `-Wimplicit-function-declaration` for stdio/time symbols; host gcc
  had accepted the same file.
- **Cause:** transitive includes on host glibc; NDK headers are
  stricter.
- **Fix:** explicit `#include <stdio.h>` and `#include <time.h>` in
  `hydra_jni.c`.
- **Prevention:** always include what you use (IWYU discipline); NDK
  clang is effectively an extra lint stage.

## 2026-10-01 — AGP used NDK 26.1 while 26.3 was installed

- **Symptom:** Gradle configured CMake against
  `…/ndk/26.1.10909125` — not installed → build failed.
- **Cause:** AGP's default `ndkVersion` doesn't match what SDK Manager
  installed.
- **Fix:** pin `ndkVersion = "26.3.11579264"` in `defaultConfig`.
- **Prevention:** never rely on AGP's NDK default; pin the exact
  installed version.

## 2026-10-01 — Emulator: `Unknown AVD name [hydra_test]`

- **Symptom:** avdmanager created the AVD, but `emulator -avd` failed.
- **Cause:** AVD written to `/root/.android/avd`; emulator looked at
  `$HOME/.android/avd` where `$HOME` differed in the environment.
- **Fix:** export `ANDROID_SDK_HOME=/root` and
  `ANDROID_AVD_HOME=/root/.android/avd` before launching.
- **Prevention:** on headless systems always set `ANDROID_AVD_HOME`
  explicitly.

## 2026-10-01 — Emulator: `libX11.so.6: cannot open shared object file`

- **Symptom:** emulator binary aborted at startup on the headless
  container.
- **Cause:** X11/GL runtime libraries not installed.
- **Fix:** `apt-get install -y libx11-6 libxext6 libxrandr2 libxrender1
  libgl1 libpulse0 libnss3 libxcb1 libxkbcommon0 libasound2`.
- **Prevention:** `-no-window` does not remove the dynamic dependency
  on X11/GL; install them in any headless CI image that runs emulators.

## 2026-10-01 — No `/dev/kvm` in container

- **Symptom:** hardware acceleration unavailable for the emulator.
- **Fix:** run with `-no-accel -gpu swiftshader_indirect` and an x86_64
  system image; boots in ~1–2 min. Avoid ARM images (QEMU TCG, very
  slow).
- **Prevention:** check `ls /dev/kvm` first and choose the emulation
  strategy accordingly.

## 2026-10-01 — OOB read behind mmap (PoC-confirmed, engine core)

- **Symptom:** crafted header with `layers*dim > weights_len` was
  accepted; `step()` read up to ~4 GiB past the file mapping.
- **Cause:** loader validated `weights_len` against file size, but
  `step()` iterates `layers × dim` packed bytes and ignored
  `weights_len`.
- **Fix:** loader now rejects `layers*dim > weights_len` (error -10);
  regression test added.
- **Prevention:** validate every field that derives an access length
  against the actual buffer/file bounds — and write a PoC to prove the
  fix (the PoC became the regression test).

## 2026-10-01 — State update: modulo-127 saturation was dead code

- **Symptom:** code review finding — `% 127` already bounds to
  [-126,126], the subsequent clamps were unreachable; distribution was
  skewed.
- **Fix:** clean clamping without modulo.
- **Prevention:** arithmetic review of "clever" wrapping; prefer
  explicit clamps.

## 2026-10-01 — Tautological determinism test (test suite quality)

- **Symptom:** `out == out2` for same input on a deterministic
  algorithm could never fail — a fake test.
- **Fix:** hand-computed expected values + seeded LCG roundtrip with
  cross-seed variance assertion (PR #3).
- **Prevention:** every test must be able to fail; review tests with
  the question "what code change would this catch?".

## 2026-10-01 — JSON on stdout polluted by loader printf

- **Symptom:** `hydra-run --json` output started with
  `[Hydra] Modell erfolgreich gemappt! …` before the JSON.
- **Cause:** library-level `printf` in `hydra_engine_load` went to
  stdout.
- **Fix:** library logs moved to stderr exclusively.
- **Prevention:** libraries never write to stdout; stdout is a data
  channel only.
