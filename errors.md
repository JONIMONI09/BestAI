# Error Log — Hydra-Stone

> Protocol: see `.claude/skills/session-workflow/SKILL.md`.
> Every error gets an entry immediately, with symptom, cause, fix and
> prevention. Newest entries at the top.

---

## 2026-10-02 — Linter audit: choosing a tool means proving it can fail

- **Symptom:** the request was "the best linters of 2026". Without a
  counter-test you risk adopting a tool that finds nothing or only
  produces noise — and then claiming it as a safeguard.
- **Approach:** every tool was installed locally, run, and verified
  with a **negative control** (temporarily revert a known fix):
  | Tool | Negative control | Finding on the real code |
  |---|---|---|
  | `gcc -fanalyzer` | clean | 0 findings |
  | `clang --analyze` | clean | 0 findings |
  | clang-tidy `bugprone`/`cert`/`concurrency` | 3 real hits | 0 after the fix |
  | cppcheck | clean | 0 (found a `%u`/signed issue in an earlier session) |
  | flawfinder `-m 2` | 29 hits, all `fopen`/`mkstemp`/char arrays | false positives; `-m 4` gives 0 |
  | **ASan + UBSan** | **`signed integer overflow: 2147483640 + 127`** in `hydra_engine_step` | 0 findings |
  | Android Lint | 5 warnings | "No issues found" after the fix |
  | ESLint 10 | 7 errors | 0 after the fix |
- **Fix / insight:** only tools whose falsifiability is demonstrated were
  adopted. The sanitizers became the most important gate because they
  are the **only** ones that report actual arithmetic UB instead of
  merely suspecting it structurally. clang-tidy was reduced to a curated
  check list (style checks produce only noise, `cert-err33-c` fires on
  every `printf` return).
- **Prevention:** rule R27 — every new lint gate must fail once against
  a reverted fix before it counts as effective. Otherwise it is
  decoration, i.e. the opposite of a fake.

## 2026-10-02 — A symlink in the model path bypassed the web server's path check

- **Symptom (verified with curl):** `models/evil.hydra` as a symlink to
  `/etc/hostname` passed `safeModelPath()`; the engine opened the target
  file and the HTTP response revealed its size:
  `500 {"error":"Command failed: ... [Hydra] file too small for header (9 bytes)"}`.
- **Cause:** `path.resolve()` is purely *lexical*. It does not follow
  symlinks — the prefix check against `ROOT` therefore only inspects the
  name, not the target. Second, separate cause for the detailed output:
  `server.js` passed `e.message` to the client unfiltered.
- **Fix:** `safeModelPath()` additionally resolves the path via
  `fs.realpathSync()`, checks the **result** against `ROOT` again and
  requires a regular file via `lstatSync().isFile()`. `src/hydra_engine.c`
  additionally opens with `O_NOFOLLOW` — that barrier belongs in the
  engine, because the API is also reachable from JNI. Error responses
  now emit only `{"error":"internal error"}`; details go to the server
  log.
- **Verification:** symlink pointing outside → 400; symlink inside
  `ROOT` → 200; real file → 200. Regression test
  `test_engine_rejects_symlink()` fails without `O_NOFOLLOW` (negative
  control executed).
- **Prevention:** a path check against a directory must check *the
  resolved target*, not the supplied name (CWE-59, symlink following).

## 2026-10-02 — Server leaked internal engine details to the client

- **Symptom (verified):** `/api/model` on a broken model returned
  `500 {"error":"Command failed: /path/hydra-run /path/model.hydra 0 1 --json\n[Hydra] ..."}` —
  server paths, engine arguments and internal messages in the response
  body.
- **Cause:** the generic `catch` passed `e.message` straight through.
- **Fix:** `console.error` to the server log, the response is a generic
  `internal error`; `execFile` errors carry the engine stderr in
  `e.detail` instead of in the response.
- **Prevention:** internal error details belong in the log, not in the
  response. Review every 5xx response as a potential information leak.

## 2026-10-02 — Android Lint found 5 real quality defects

- **Symptom:** `gradle lintDebug` reported 0 errors but 5 warnings.
- **Findings:** no app icon (`MissingApplicationIcon`), four
  `SetTextI18n` (hardcoded UI strings in Kotlin). After the icon fix
  more appeared: `LockedOrientationActivity`/`DiscouragedApi`
  (forced portrait orientation), `PluralsCandidate`,
  `TypographyEllipsis`, `DataExtractionRules`, `MonochromeLauncherIcon`.
- **Fix:** icon as vector (adaptive from API 26, fallback shape from
  API 24, monochrome layer for Android 13+), all UI strings moved to
  `res/values/strings.xml`, `plurals` for byte/iteration counts,
  orientation lock removed (the UI is laid out vertically and works in
  both orientations), `data_extraction_rules.xml` + `backup_rules.xml`
  disable cloud backup explicitly, ellipsis characters.
- **Result:** Android Lint reports **"No issues found"**, and the CI gate
  fails on every regression (grep for `errors?, [1-9][0-9]* warnings?`).
- **Prevention:** lint warnings are not noise when they report a real
  omission (no icon = the app looks broken in the launcher).

## 2026-10-02 — ESLint found six leaks into the global scope

- **Symptom:** `public/app.js` declared six functions and `$()` at
  global level (`no-implicit-globals`) — collision risk with any other
  script on the page.
- **Cause:** no module wrapper; classic script frontend.
- **Fix:** the whole logic wrapped in an IIFE. Additionally `catch (e)`
  in `server.js` changed to an optional catch binding (ESLint
  `no-unused-vars`).
- **Verification:** the wiring after wrapping was checked with a minimal
  DOM shim (handlers `run`, `humanity`, `keydown` all registered); a
  negative control with injected `eval`/`var`/`==` made ESLint fail
  correctly.
- **Prevention:** browser scripts without a module system need an IIFE.

---

## 2026-10-02 — NEON kernel inverted the sign of every ternary weight

- **Symptom:** x86 and ARM produced *different* token sequences for the
  same model. `hydra-run models/demo.hydra 42 8 --json` gave
  `211,388,401,330,...` on x86 and `211,28,137,170,...` under ARM/NEON
  (reproduced with `aarch64-linux-gnu-gcc-12 -static` + `qemu-aarch64-static`).
- **Cause:** in `src/hydra_neon.h` the decode was
  `vsubq_s8(vreinterpretq_s8_u8(p1), vreinterpretq_s8_u8(n1))`.
  `vceqq_u8` returns `0xFF` on a match, which reinterpreted as `int8_t`
  is **-1**, so code `01` (+1) decoded as `0xFF - 0x00 = -1`. Every
  weight had the opposite sign on ARM.
- **Fix:** normalise the comparison masks to `0x01` first and subtract in
  the unsigned domain, so the byte is literally `0x01` (+1) or `0xFF`
  (-1): `vreinterpretq_s8_u8(vsubq_u8(p1, n1))`.
- **Prevention:** `test_neon_matches_scalar()` runs a scalar reference
  `step()` next to the NEON kernel **inside the same ARM build** and
  demands bit-identical token sequences *and* state vectors. Reviewing
  each path against fixed expectations is not enough — the two paths
  must be compared to *each other*. Independently confirmed on three
  targets: host x86-64, ARM64/NEON, and the Android emulator all emit
  `211,36,185,410,7,40,197,478`.

## 2026-10-02 — NEON store offsets skipped lanes 8..15

- **Symptom:** the new `test_neon_matches_scalar` failed on ARM:
  *state vector* mismatch while the token sequence matched. Observed
  under qemu: ARM state `0,-11,-33,-11,...` vs. expected `0,-11,0,0,-33,...`.
- **Cause:** when the accumulator was widened to `int64_t`, the store
  offsets were scaled by 2 (`acc + 0/2/4/6`) even though each call now
  advances 4 lanes instead of 2. Lanes 8..15 were never written and the
  second half of `sum_lo` overwrote part of the first half.
- **Fix:** offsets `0 / 4 / 8 / 12`; `sum_lo` covers lanes 0–7, `sum_hi`
  covers lanes 8–15.
- **Prevention:** the same-build comparison test caught this within one
  run — it would have shipped otherwise, because no cross-platform
  comparison existed before.

## 2026-10-02 — Signed integer overflow via unbounded `layers` (UB)

- **Symptom/PoC:** `dim=1, vocab=128, layers=16909321, weights_len=16909321,
  weights_offset=24`, every weight byte `0x01`, token `127`. Each layer
  adds `+127`; after 16 909 320 layers the accumulator is 2 147 483 640
  and the next addition crosses `INT32_MAX` — signed overflow, i.e.
  undefined behaviour. The file is only ~16.9 MB.
- **Cause:** the loader bounded `dim` but not `layers`.
- **Fix:** `HYDRA_MAX_LAYERS 4096` enforced at load (error `-11`), plus
  an `int64_t` accumulator in `step()` and in the NEON kernel as defence
  in depth. `4096 × 254 = 1 040 384` keeps even an `int32` safe.
- **Prevention:** the PoC became `test_engine_rejects_layer_overflow()`,
  which writes the full 16.9 MB of real `0x01` bytes so it also fails if
  someone merely removes the `layers × dim ≤ weights_len` check.
  Rule: bound every field that multiplies into an accumulator length.

## 2026-10-02 — `token_in & 0x7F` collapsed the vocabulary

- **Symptom:** with `vocab_size = 1024`, every eighth token ID produced
  an identical weight contribution — only 128 of 1024 tokens were
  reachable.
- **Cause:** an overflow guard for the old `int16` NEON path that was
  never revisited once the accumulator bounds changed.
- **Fix:** mask removed. With `vocab ≤ 1024` and `|state| ≤ 127` the
  per-lane product is ≤ 1150, comfortably inside `int16`. The old
  behaviour stays available behind `-DHYDRA_TOKENV_MASK`.
- **Prevention:** `test_token_mask_collisions()` asserts `state[0] == 127`
  after stepping token 128 — with the mask it would be `0`.

## 2026-10-02 — `labs()` destroyed the sign of the accumulator

- **Symptom:** weights `w = +1` and `w = −1` yielded the *same* output
  token (`w=0x01, token=5` and `w=0x02, token=5` both gave `11`), i.e.
  half the ternary alphabet was inert.
- **Cause:** `labs((long)accumulator[0])` takes the absolute value.
- **Fix:** symmetric modulo — `raw = acc % vocab; if (raw < 0) raw += vocab;
  out = (raw + token + 1) % vocab`.
- **Prevention:** the expected value for `w=0x02` in
  `test_decoder_known_values` changed from `11` to `1` **on purpose** and
  carries an explanatory comment. A changed expectation is a decision to
  review, not something to apply silently.

## 2026-10-02 — `weights_offset` was allowed to point into the header

- **Symptom:** a formally accepted model whose `weights_offset` was `8`
  would execute header bytes as ternary weights.
- **Cause:** only `weights_offset + weights_len ≤ file_size` was checked.
- **Fix:** `weights_offset >= sizeof(HydraModelHeader)` (error `-12`).
- **Prevention:** `test_engine_rejects_offset_in_header()`.

## 2026-10-02 — Double-close after a failed load

- **Symptom:** a loader failure closed `engine->fd` but left it set, so a
  caller's subsequent `hydra_engine_unload()` closed the same descriptor
  again (`EBADF`, or an unrelated fd closed by mistake).
- **Cause:** repeated `close(engine->fd); return -N;` in every branch.
- **Fix:** one `goto fail` label that sets `fd = -1`, calls
  `hydra_engine_unload()` and returns the code.
- **Prevention:** `test_engine_rejects_garbage()` now asserts `e.fd == -1`
  after a failed load and that a following `unload()` leaves it at `-1`.

## 2026-10-02 — "Zero-RAM" was an unproven claim

- **Symptom:** README/ARCHITECTURE claimed "inference-time RAM usage is
  constant regardless of model size" and "Zero-RAM".
- **Cause:** `madvise(MADV_SEQUENTIAL)` is a *read-ahead* hint, not an
  eviction hint. Touched pages become resident in the page cache, and
  because `step()` walks the entire weight region per token, every token
  re-faults them after eviction.
- **Fix:** both documents now state precisely what holds (no heap
  allocation, O(1) engine memory, clean reclaimable pages) and what does
  not (a zero resident set is impossible). Added opt-in
  `-DHYDRA_DROP_CACHE` issuing `madvise(MADV_DONTNEED)` after each step.
- **Prevention:** Rule R6 extended — marketing-style claims must name
  the mechanism that makes them true.

## 2026-10-02 — NaN bypassed the coexistence axiom

- **Symptom:** `hydra_verify_axiom(NAN, 99.5f, &s)` returned `rc = 1`
  (allowed) — the safety gate was bypassed.
- **Cause:** in IEEE-754, `NaN <= 0.0f` is `false`, so the only
  comparison in the function did not trigger. A non-finite
  `proposed_score` likewise produced a NaN/Inf result.
- **Fix:** `!isfinite(...) || ... <= 0.0f` for `humanity_factor`, and a
  separate `!isfinite(proposed_score)` rejection; both push the score to
  `-1e9`.
- **Prevention:** `test_axiom()` covers NaN, `+Inf` humanity and
  non-finite score. Rule: a safety gate must reject non-finite input
  explicitly, never rely on comparison operators alone.

## 2026-10-02 — Format was not endian-portable

- **Symptom:** the header was `memcpy`-ed into native integers although
  `docs/FORMAT.md` defines little-endian; on a big-endian host every
  field would decode incorrectly and loading would fail.
- **Cause:** convenience over an explicitly specified byte order.
- **Fix:** `rd_u16le` / `rd_u32le` in the loader; the tests write with
  explicit `wr_u16le` / `wr_u32le` so they stay meaningful on any host.
- **Prevention:** byte-level format docs and the test writers must agree;
  no struct `memcpy` for on-disk headers.

## 2026-10-02 — Tests wrote to fixed, symlink-vulnerable `/tmp` paths

- **Symptom:** `/tmp/hydra_test_model.hydra` and friends were recreated
  with `fopen(..., "wb")` on every run.
- **Cause:** convenience. Parallel runs collide, and in an untrusted
  `/tmp` the fixed names are a symlink-attack target.
- **Fix:** `mkstemp()` (unpredictable name, mode 0600) with `unlink()`
  in the cleanup of every test.
- **Prevention:** never open a predictable path for writing in a test.

## 2026-10-02 — CLI silently discarded surplus arguments

- **Symptom:** `hydra-run model 1 2 3` ignored the trailing `3` without
  a word, and `strtol` was used without checking `errno`, so a
  20-digit number was silently clamped to `LONG_MAX`.
- **Fix:** `--help` / `-h`, rejection of unknown options, explicit
  rejection of surplus positional arguments, and an `errno == ERANGE`
  check in `parse_long_arg()`.
- **Prevention:** a CLI must fail loudly on input it does not understand.

## 2026-10-02 — Web console: 500 on malformed body, silent model fallback

- **Symptom (verified with curl):** `POST /api/infer` with body `null`
  answered `500 {"error":"Cannot read properties of null (reading 'token')"}`
  — an internal TypeError message leaked to the client. A body of
  `[1,2]` was accepted. An explicitly requested but rejected model path
  (`../../etc/passwd.hydra`) silently fell back to the default model and
  returned `200` with results for a *different* model.
- **Cause:** no body-shape validation, and `safeModelPath(...) || DEFAULT_MODEL`
  conflates "not supplied" with "supplied and invalid".
- **Fix:** explicit JSON parse (400), object-shape check (400), finite
  numeric validation for `token`/`steps` (400), and an explicit 400 for a
  supplied-but-invalid path.
- **Not vulnerable (checked, no finding):** command injection — the model
  path is passed to `execFile` as an argument with `shell: false`, never
  through a shell; path traversal on the static handler is blocked by
  `path.normalize` plus a `PUBLIC + path.sep` prefix check (verified:
  `/../Makefile` and `/%2e%2e%2fMakefile` both return 404).

## 2026-10-02 — JNI: unchecked callback exception, negative `startToken`

- **Symptom:** `emit_token()` called back into Kotlin without checking
  `ExceptionCheck()`. A throwing callback leaves the exception pending,
  which makes every subsequent JNI call undefined behaviour. Separately,
  `startToken = -5` produced `(-5 % 512) = -5` → `(uint16_t)65531`, a
  valid but semantically wrong seed that was never rejected.
- **Fix:** `emit_token()` returns `-1` on a pending exception (logged and
  cleared), and the inference loop aborts with a JSON error. Negative
  `startToken` is rejected up front.
- **Prevention:** JNI callbacks must check for pending exceptions, and
  every numeric crossing the JNI boundary needs an explicit range check.
  Same class as the "copy data before `*_unload`" bug already logged
  above: mobile-only defects, invisible to host tests (Rule R20).

---

## 2026-10-01 — Skills still "Not loaded" after frontmatter fix: non-ASCII in description

- **Symptom:** After adding valid-looking frontmatter, the Skills UI
  still showed every skill as `Not loaded` with
  *"Freebuff could not read this SKILL.md. The frontmatter needs
  `name: <dir>` (matching the skill's own folder name)"* — even though
  `name` matched and the YAML parsed locally.
- **Cause (most probable):** the `description:` values contained the
  em dash `—` (non-ASCII) inside an unquoted plain YAML scalar; the
  UI's parser is stricter than standard YAML. Also possible: UI cache
  lag after the merge.
- **Fix:** rewrote all three frontmatters as pure ASCII with quoted
  description scalars:
  ```yaml
  description: "...ascii-only, colon inside quotes..."
  ```
  Validation script now additionally asserts `fm_raw.isascii()`.
- **Prevention:** Rule R21 extended — frontmatter must be pure ASCII
  with quoted description; the validator enforces it before commit.

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

## 2026-10-02 — CI configuration errors (three red gates despite green code quality)

- **Symptom:** after the first `lint.yml` run three jobs were red even
  though every static analyzer was green locally — a hint at
  configuration rather than code errors.
  (a) `Android lint`: `android-actions/setup-android@v3` runs
      `sdkmanager tools` — the package was removed from the repository:
      `Warning: Failed to find package 'tools'` → exit 1.
  (b) `Semgrep`: `semgrep ci --error` — the option does not exist;
      Semgrep already exits with 1 on findings by itself.
  (c) `CodeQL (javascript-typescript)`: `build-mode: autobuild` is
      rejected for JS/TS; the correct value is `build-mode: none`.
- **Cause:** third-party actions and CLI flags were written from memory
  instead of checked against the current docs; a deprecated action keeps
  referencing a retired package name even after updates.
- **Fix:** (a) download/unpack the cmdline-tools explicitly, set PATH
  and license acceptance explicitly — the same sequence proven locally
  in `docs/ANDROID_SKILL.md`; (b) removed `--error`, added
  `--suppress-errors`; (c) `build-mode: none` for both languages.
- **Prevention:** check every external action and every CLI flag against
  the official docs before committing (R26). Additionally simulate
  workflow steps locally before they go into the PR.

## 2026-10-02 — "OldTargetApi" only in CI, not locally (runner SDK)

- **Symptom:** after the SDK fix `Android lint` ran through but reported
  `0 errors, 1 warnings` with
  `Warning: Not targeting the latest versions of Android [OldTargetApi]`
  and the gate (grep "No issues found") turned red.
- **Cause:** the hosted runner ships a prebuilt SDK that additionally
  contains `platforms;android-35` and `android-36`. Lint judges
  `OldTargetApi` against the platforms that are *present*. Locally only
  `android-34` is installed — there lint reports `No issues found`.
  Same toolchain, two results: the warning therefore came from the
  runner environment, not from the project.
- **Fix:** both workflows set up their own SDK root
  (`$RUNNER_TEMP/android-sdk`) and install nothing but the pinned
  packages there. Additionally `release.yml` derives the build-tools
  paths from `ANDROID_HOME` instead of a hardcoded
  `/opt/android-sdk`, which does not exist on the runner.
- **Prevention:** for "only in CI" findings, first compare the runner
  environment against the local toolchain (differential analysis) before
  changing code or lint configuration. A gate must never be cosmetically
  weakened to hide an environment problem.

## 2026-10-02 — Manual release was unusable (mandatory tag input, silent wrong version)

- **Symptom:** `release.yml` had a `workflow_dispatch`, but with
  `required: true` for the tag input. A manual run was therefore only
  possible after typing an existing tag by hand — i.e. not testable
  without a new push. A run without input (e.g. via API) additionally
  produced `TAG=main`, `version=main`, `versionCode=0`: the arithmetic
  `$(( MAJ * 10000 ))` evaluates an unknown word as 0 instead of
  reporting an error.
- **Cause:** the input was modelled as a mandatory field ("Existing tag
  to publish a release for") although the actual purpose is a
  *testable* build. In addition the tag format was never validated and
  `versionCode` was never checked against 0.
- **Fix:** `tag` is optional, `publish` is a boolean (default
  `false`). Without a tag a dry run runs under the synthetic tag
  `v0.0.0-ci.<run_number>`, the `publish` job is disabled via
  `if: needs.version.outputs.publish == 'true'`. The tag is validated
  against `vMAJOR.MINOR.PATCH` (an error instead of a release named
  "main"), `versionCode` is at least 1, and all inputs reach the script
  via `env:` instead of `${{ }}` (no shell-injection risk). The tag-push
  trigger remains unchanged.
- **Prevention:** every dispatch path needs a negative control.
  `tools/ci_release_version_test.sh` replays the version logic straight
  from the YAML (tag push, manual publish, dry run, invalid tag) and runs
  as job `release-config` in `lint.yml`. Negative control executed: with
  the `publish` output removed the test reports FAILED (exit 1) — the
  gate can therefore actually turn red.

## 2026-10-02 — Workflow dispatch via API impossible (403, app permissions)

- **Symptom:** `gh workflow run release.yml --ref <branch>` answered with
  `HTTP 403: Resource not accessible by integration`.
- **Cause:** the managed GitHub app credential may read workflows but
  cannot create a `workflow_dispatch` event — the app lacks the Actions
  write permission. This is not a property of the repository and cannot
  be fixed inside the workflow.
- **Fix:** start *Run workflow* manually in the Actions tab — that uses
  the permissions of the logged-in user. If the app should be able to
  test the dispatch itself in future, its permission has to be raised to
  `Actions: write`.
- **Prevention:** verify separately those CI artefacts that an app
  without Actions write permission cannot produce (dispatch, re-run,
  release publish): logic locally, trigger manually, publish path only
  after explicit approval.

## 2026-10-02 — Skills UI reports "Not loaded" despite correct frontmatter

- **Symptom:** the Skills UI shows all three skills as *Not loaded* with
  `The frontmatter needs name: <skill-dir> (matching ...)`.
- **Cause:** the message names the **expected** name per skill, i.e. the
  parser sees the files but rejects them. A byte-level check on disk
  **and** on `origin/main` showed: no BOM, no CRLF, no tabs,
  `name == directory name`, `description` in quotation marks and 100 %
  ASCII, exactly two keys. The frontmatter is therefore formally
  correct — the message is a **cache/display state of the UI**, not a
  file defect.
- **Fix:** no code fix needed; the skills are valid on `main`. Reload
  the UI or reconnect the workspace.
- **Prevention:** for frontmatter errors, check the bytes first
  (BOM/CRLF/tabs) and read the expected-name source — the error message
  names the target and therefore reveals whether the parser or the file
  is the problem.
## 2026-10-02 — The APK was only ever built on a tag

- **Symptom:** `main` was green on every check, but the Android app was
  never built on a normal push and the repository showed "No releases
  published".
- **Cause:** `release.yml` triggers only on `v*.*.*` tags (plus the
  manual dispatch), and `ci.yml` had no Android job at all — the
  `android-lint` job builds only what lint needs, not a signed APK. So
  a broken Gradle/NDK build would have surfaced first at release time,
  and no tag had ever been pushed.
- **Fix:** new `android-apk` job in `ci.yml` runs on every push and pull
  request: `assembleDebug` + `assembleRelease`, `apksigner verify`, ABIs
  and asset checked with `aapt`, APK uploaded as an artifact. The tag
  push remains the only trigger that publishes a release.
- **Prevention:** every shippable artifact must be produced by a
  non-release job as well. A gate that only runs on release day is not a
  gate, it is a surprise.

## 2026-10-02 — JNI bridge had no machine-checked contract

- **Symptom:** `HydraBridge.kt` declares `external fun runInference(...)`
  and `hydra_jni.c` implements `Java_dev_hydrastone_HydraBridge_runInference`,
  but nothing verified that the two still match. Renaming one side would
  only fail at runtime on a device.
- **Cause:** no test in the chain looked at the JNI symbol names; the
  existing Android gates compile both sides independently.
- **Fix:** `tools/jni_signature_check.sh` generates an equivalent Java
  class from the Kotlin declarations, runs `javac -h` on it and compares
  the generated header against the C file — symbol name and parameter
  list. It runs as job `jni-signatures`. The check also surfaced a real
  discrepancy: javac derives the receiver as `jobject` (a Kotlin
  `object` member is an instance method) while the C bridge declares
  `jclass`. It is harmless — the parameter is unused — and the script
  reports it as a `note` instead of hiding it.
- **Prevention:** derive expectations with the same toolchain that will
  consume them (`javac -h`), never by hand. Negative control executed:
  an extra unimplemented method makes the gate exit 1.

## 2026-10-02 — CI output and release notes were still German

- **Symptom:** after the documentation was translated, the CI step names,
  `::error::`/`::notice::` messages, the workflow comments and the entire
  release notes body were still German — a repository that presents
  itself as an English project published English docs and German release
  notes.
- **Cause:** the translation pass covered tracked `.md` files only; the
  YAML files were not part of that inventory.
- **Fix:** `lint.yml`, `release.yml` and `vibeworks-check.yml` translated
  (comments, step names, error messages, release body). `ci.yml` was
  already English.
- **Prevention:** when switching the documentation language, grep the
  whole repository including workflows — the user-visible text is what
  counts, not only the files with a `.md` suffix.

## 2026-10-02 — Every CI-built APK shipped without the model asset

- **Symptom:** the new `android-apk` job failed on its first run even
  though all three ABIs were built and packaged. The failure was not an
  ABI: it was `grep -q 'assets/demo.hydra'`.
- **Cause:** `android/app/src/main/assets/demo.hydra` existed on the
  developer machine but was never tracked by git. Every clean checkout —
  i.e. every CI run and every release run — therefore built an APK
  without the demo model, and the app would have failed at runtime with
  "model file not found". The old `release.yml` had the same
  `grep -q 'assets/demo.hydra'` check, so the first real tag run would
  have failed the same way.
- **Fix:** the asset is now tracked, and `tools/make_dummy_model.py` is
  deterministic (three runs, one sha256), so CI regenerates it and
  compares it with the committed file — gate 1e in `ci.yml`. The
  artifact upload also runs on failure so a broken APK can still be
  inspected.
- **Prevention:** every file the packager reads has to be tracked. Ask
  what a clean checkout contains, not what the workstation contains; a
  build that only works locally is not a build.

## 2026-10-02 — First real tag run failed in two jobs

- **Symptom:** `v1.0.0` was pushed and `release.yml` ran for the first
  time ever. Two jobs failed: `CLI (Linux)` with
  `cp: cannot create regular file 'dist/hydra-run-linux-x64': No such
  file or directory` and `Android APK` with
  `path may not be null or empty string. path=''` from
  `build.gradle.kts` line 50. No release was published.
- **Cause:** (a) the Linux job copies into `dist/` but only the macOS
  job created the directory. (b) A GitHub secret that is not configured
  is exposed as an EMPTY environment string, not as `null`, so
  `file(System.getenv("HYDRA_KEYSTORE")!!)` called `file("")` and threw.
  Every local and CI run had the variable unset (real `null`), which is
  why the bug survived 20 green runs.
- **Fix:** `mkdir -p dist` in the Linux job; the Gradle script now
  resolves the keystore path once via
  `System.getenv("HYDRA_KEYSTORE")?.takeIf { it.isNotBlank() }` and uses
  that for both the signing config and the build type; the workflow also
  only exports `HYDRA_KEYSTORE` when the secret really carries data.
- **Prevention:** test the *unset* code path the way production
  presents it. An empty environment string is not the same as an absent
  variable, and a release-only code path is not exercised by a normal
  push. Both fixes were replayed locally with `HYDRA_KEYSTORE=""`
  before pushing.
