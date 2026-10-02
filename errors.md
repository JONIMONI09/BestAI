# Error Log — Hydra-Stone

> Protocol: see `.claude/skills/session-workflow/SKILL.md`.
> Every error gets an entry immediately, with symptom, cause, fix and
> prevention. Newest entries at the top.

---

## 2026-10-02 — Linter-Audit: Werkzeug auswaehlen heisst Falsifizierbarkeit pruefen

- **Symptom:** Die Anfrage lautete "die besten Linter 2026". Ohne
  Gegentest laeuft man Gefahr, ein Werkzeug aufzunehmen, das nichts findet
  oder nur Rauschen produziert — und es dann als Absicherung zu behaupten.
- **Vorgehen:** jedes Werkzeug zuerst lokal installiert, laufen gelassen
  und mit einer **Negativkontrolle** geprueft (bekannten Fix
  temporaerreueckbauen). Ergebnis:
  | Werkzeug | Negativkontrolle | Befund am echten Code |
  |---|---|---|
  | `gcc -fanalyzer` | sauber | 0 Befunde |
  | `clang --analyze` | sauber | 0 Befunde |
  | clang-tidy `bugprone`/`cert`/`concurrency` | 3 echte Treffer | nach Fix: 0 |
  | cppcheck | sauber | 0 (fand `%u`/signed in einer frueheren Session) |
  | flawfinder `-m 2` | 29 Treffer, alle `fopen`/`mkstemp`/char-Array | Fehlalarme; `-m 4` ergibt 0 |
  | **ASan + UBSan** | **`signed integer overflow: 2147483640 + 127`** in `hydra_engine_step` | 0 Befunde |
  | Android Lint | 5 Warnings | nach Fix: „No issues found" |
  | ESLint 10 | 7 Fehler | nach Fix: 0 |
- **Fix / Erkenntnis:** nur die Werkzeuge uebernommen, deren
  Falsifizierbarkeit belegt ist. Sanitizer wurde zum wichtigsten Gate,
  weil es als **einziges** die tatsaechliche Arithmetic-UB meldet statt
  sie nur strukturell zu vermuten. clang-tidy wurde auf eine kuratierte
  Checkliste reduziert (Style-Checks erzeugen nur Rauschen,
  `cert-err33-c` feuert auf jeden `printf`-Return).
- **Praeventiv:** Regel R27 — jedes neue Lint-Gate muss einmal an einem
  zurueckgebauten Fix scheitern, bevor es als wirksam gilt. Sonst ist es
  Dekoration, KEIN Fake in der anderen Richtung.

## 2026-10-02 — Symlink im Modellpfad umging die Pfadpruefung des Web-Servers

- **Symptom (verifiziert mit curl):** `models/evil.hydra` als Symlink auf
  `/etc/hostname` passierte `safeModelPath()`; die Engine oeffnete die
  Zieldatei und die HTTP-Antwort verriet deren Groesse:
  `500 {"error":"Command failed: ... [Hydra] Datei zu klein fuer Header (9 Bytes)"}`.
- **Cause:** `path.resolve()` ist rein *lexikalisch*. Es folgt Symlinks
  nicht — der Prefix-Check gegen `ROOT` prueft also nur den Namen, nicht
  das Ziel. Dazu:getrennte Ursache fuer die Detailausgabe: `server.js`
  gab `e.message` ungefiltert an den Client.
- **Fix:** `safeModelPath()` loest den Pfad zusaetzlich ueber
  `fs.realpathSync()` auf, prueft das **Ergebnis** erneut gegen `ROOT` und
  verlangt per `lstatSync().isFile()` eine regulaere Datei.
  `src/hydra_engine.c` oeffnet zusaetzlich mit `O_NOFOLLOW` — die Schranke
  gehoert in die Engine, weil die API auch aus JNI heraus erreichbar ist.
  Fehlerantworten geben nur noch `{"error":"internal error"}` aus, Details
  gehen ins Server-Log.
- **Verifikation:** Symlink nach draussen -> 400; Symlink innerhalb von
  `ROOT` -> 200; echte Datei -> 200. Regressionstest
  `test_engine_rejects_symlink()` schlaegt ohne `O_NOFOLLOW` fehl
  (Negativkontrolle ausgefuehrt).
- **Praeventiv:** Pfadpruefung gegen ein Verzeichnis muss *das aufgeloeste
  Ziel* pruefen, nicht den eingegebenen Namen (CWE-59, Symlink-Following).

## 2026-10-02 — Server verriet interne Engine-Details an den Client

- **Symptom (verifiziert):** `/api/model` auf ein defektes Modell lieferte
  `500 {"error":"Command failed: /pfad/hydra-run /pfad/modell.hydra 0 1 --json\n[Hydra] ..."}` —
  Serverpfade, Engine-Argumente und interne Meldungen im Antwort-Body.
- **Cause:** der generische `catch` gab `e.message` direkt weiter.
- **Fix:** `console.error` ins Server-Log, Antwort ist ein generisches
  `internal error`; `execFile`-Fehler tragen Engine-stderr in
  `e.detail` statt in die Antwort.
- **Praeventiv:** interne Fehlerdetails gehoeren ins Log, nicht in die
  Antwort. Jede 5xx-Antwort als Informationsleck-Potential pruefen.

## 2026-10-02 — Android Lint fand 5 echte Qualitaetsmaengel

- **Symptom:** `gradle lintDebug` meldete 0 Errors, aber 5 Warnings.
- **Befunde:** kein App-Icon (`MissingApplicationIcon`), vier
  `SetTextI18n` (hart kodierte UI-Texte in Kotlin). Nach dem Icon-Fix
  kamen weitere hinzu: `LockedOrientationActivity`/`DiscouragedApi`
  (erzwungene Portrait-Orientierung), `PluralsCandidate`,
  `TypographyEllipsis`, `DataExtractionRules`, `MonochromeLauncherIcon`.
- **Fix:** Icon als Vektor (adaptiv ab API 26, Fallback-Shape ab API 24,
  monochrome-Layer fuer Android 13+), alle UI-Texte nach
  `res/values/strings.xml`, `plurals` fuer Byte-/Iterationszaehle,
  Orientierungssperre entfernt (die UI ist vertical aufgebaut und
  funktioniert in beiden Richtungen), `data_extraction_rules.xml` +
  `backup_rules.xml` schalten Cloud-Backup explizit ab, Ellipsis-Zeichen.
- **Ergebnis:** Android Lint meldet **„No issues found"**, und das Gate
  in der CI schlaegt bei jeder Regression fehl (grep auf
  `errors?, [1-9][0-9]* warnings?`).
- **Praeventiv:** Lint-Warnungen sind kein Rauschen, wenn sie echte
  Fehlendeinrichtung melden (kein Icon = App sieht im Launcher defekt aus).

## 2026-10-02 — ESLint fand sechs leaks in den globalen Scope

- **Symptom:** `public/app.js` deklarierte sechs Funktionen und `$()` auf
  globaler Ebene (`no-implicit-globals`) — Kollisionsrisiko mit jedem
  anderen Skript auf der Seite.
- **Cause:** kein Modul-Wrapper; klassisches Skript-Frontend.
- **Fix:** gesamte Logik in eine IIFE gehüllt. Zusätzlich `catch (e)` in
  `server.js` zu optionalem Catch-Binding umgestellt (ESLint
  `no-unused-vars`).
- **Verifikation:** Wiring nach dem Wrapping mit einem minimalen DOM-Shim
  geprueft (Handler `run`, `humanity`, `keydown` alle registriert);
  Negativkontrolle mit eingefuegtem `eval`/`var`/`==` liess ESLint
  korrekt fehlschlagen.
- **Praeventiv:** Browser-Skripte ohne Modulsystem brauchen eine IIFE.

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
  *State-Vektor* mismatch while the token sequence matched. Observed
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
