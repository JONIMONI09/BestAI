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
- ✅ **2026-10-01 — CI lint stage (PR #7, OPEN, not merged by request):**
  Dual-compiler syntax gates (gcc+clang), cppcheck, Python format gate,
  error-artifact upload. cppcheck caught a real `%u`/signed issue on
  first run.

- ✅ **2026-10-02 — Bugfix audit (PR #11, offen):** NEON-Vorzeicheninversion
  (PoC: x86 `211,388,...` vs. ARM `211,28,...`) und Layer-Overflow (UB)
  gefixt, dazu 12 weitere Befunde; `test_neon_matches_scalar` vergleicht
  NEON und Skalar **im selben Build** und fand sofort einen zweiten,
  latenten Lane-Offset-Bug. 49 Tests x86 / 51 ARM.
- ✅ **2026-10-02 — Web-Console, Skills, Android-APK:** siehe
  "Done (earlier today)" und `docs/ANDROID_SKILL.md`.

## Done (earlier today)

- ✅ Skills created + protocol files (PR #8, merged by user)
- ✅ Frontmatter v1 added (PR branch), then hardening (ASCII + quoted,
  **PR #9, merged by user at 15:46:21Z**) — main now serves valid
  skills; user invoked `/android-ndk-build` successfully → skills work
- ✅ PR #7 (CI lint) — merged by user as part of #8 line? **No: PR #7
  is still OPEN** (separate lint PR), awaiting approval

## Current task

**Security/lint hardening + GitHub Release with an installable APK**,
following the CVE cross-check and linter audit from the previous pass.

## Plan

- [x] Web-Verifikation aller CVE-IDs aus dem Audit:
      CVE-2025-2439 und CVE-2025-2445 (Cortex.cpp, bestaetigt ueber
      Tenable/Snyk), CVE-2025-53630 (`gguf_init_from_file_impl`,
      NVD), CVE-2026-27940 (gleiche Funktion, spaetere Korrektur),
      CVE-2026-33298 (`ggml_nbytes`, CVE.org/Red Hat),
      CVE-2026-70638 (llama.cpp Android-JNI `new_1batch`, Builds
      b1886–b7445, NVD) — **alle IDs und Zuordnungen korrekt**
- [x] Web-Recherche zu den besten Linter 2026 (C / Android / JS)
- [x] Jedes Werkzeug lokal installiert und **mit Negativkontrolle**
      geprueft (Fix temporaer zurueckbauen, Gate muss fehlschlagen):
      - `gcc -fanalyzer`, `clang --analyze`, cppcheck → kein Befund
      - clang-tidy → 3 echte `bugprone-easily-swappable-parameters`,
        nach NOLINT mit Begruendung 0
      - flawfinder `-m 2` → 29 Fehlalarme, `-m 4` → 0
      - **ASan+UBSan → `signed integer overflow: 2147483640 + 127`**
        auf dem ungefixten Code, 0 auf dem gefixten
      - Android Lint → 5 Warnings, nach Fix „No issues found"
      - ESLint 10 → 7 Fehler, nach Fix 0
- [x] **Neue Befunde aus dem Audit reproduziert und gefixt:**
      - Symlink im Modellpfad umging `safeModelPath()`
        (verifiziert: Fehler verriet `/etc/hostname`-Groesse)
      - Server gab interne Engine-Details an den Client
      - 6 globale Funktionen im Frontend (`no-implicit-globals`)
- [x] Härtungen: `O_NOFOLLOW` im Loader + `realpath`/`lstat` im Server,
      generische 500-Antworten, IIFE im Frontend
- [x] Regressionstests: `test_engine_rejects_symlink()` (schlägt ohne
      `O_NOFOLLOW` fehl — Negativkontrolle ausgeführt)
- [x] Android: Vektor-Icon (adaptiv + Fallback + monochrome), alle
      UI-Texte als Ressourcen, `plurals`, Backup-Regeln, kein
      Orientierungs-Lock → **Android Lint: keine Befunde**
- [x] Release: signierte `assembleRelease`-Konfiguration mit
      Keystore-Secret **und** Debug-Fallback; Version aus Git-Tag
- [x] `.github/workflows/lint.yml`: 7 Jobs (c-lint, sanitizers, jni-lint,
      web-lint, android-lint, semgrep, codeql)
- [x] `.github/workflows/release.yml`: 5 Jobs, parallele Linux-/macOS-/
      Android-Builds, Paritäts-Gate, APK-Verifikation, SHA256SUMS
- [x] Release-Kette lokal komplett nachgespielt (Binaries + APK +
      Checksummen)
- [x] **Release-APK auf dem Emulator installiert und ausgeführt**:
      `adb install` → Success, Token-Sequenz
      `211,36,185,410,7,40,197,478` — identisch zu Host und ARM/NEON
- [x] `errors.md`: 5 neue Einträge; `rules.md`: R25–R30 (Linter-
      Falsifizierbarkeit, Web-Verifikation, Fehler-Leak, Symlink-Pfade)
- [x] README: Release-/APK-Abschnitt, Linter-Tabelle, Projektstruktur
- [x] Commit (kein Checkout) + PR — **kein Merge**
- [x] PR #12 und #11 vom User **gemergt** (main: `cfdb34f`) — alle
      offenen PRs sind damit geschlossen

## Runde 2026-10-02 (CI-Fix-Runde nach Merge)

- [x] Status geklaert: `main` ist gruen bis auf `Android lint`
      (13/14 Jobs gruen) — Ursache ist **nicht** der Code
- [x] Fehlerursache im Log verifiziert: `android-actions/setup-android@v3`
      ruft `sdkmanager tools` auf; das Paket existiert nicht mehr
      (`Warning: Failed to find package 'tools'` → exit 1)
- [x] Fix: cmdline-tools explizit herunterladen/entpacken + PATH,
      Lizenz-Akzeptanz explizit, in **lint.yml und release.yml**
- [x] Neuer Branch `fix/android-lint-cmdline-tools` direkt auf
      `origin/main` gebaut (Plumbing, **kein `git checkout`** — R17/R20),
      PR eroeffnet, **kein Merge** (R14)

## Status / Notes

- Der wichtigste Erkenntnis dieser Runde: **UBSan ist das einzige Gate,
  das die echte Arithmetic-UB meldet.** Alle statischen Analyseren waren
  blind dafür. Deshalb ist der Sanitizer-Job Pflicht und nicht optional.
- `flawfinder` bleibt auf Schwelle 4 — auf Schwelle 2 meldet es jeden
  `fopen`/`mkstemp` und jedes char-Array, also nur Rauschen.
- CVE-2026-70638 (llama.cpp JNI-Multiplikations-Overflow) ist die
  Fehlerklasse, nach der `hydra_jni.c` durchsucht wurde: die JNI-Brücke
 _allokiert_ nichts aus Headerfeldern, sondern clamped nur, deshalb
  nicht anwendbar.
- Offene PRs (#7 CI-Lint, #10 Skill-Optimierung, #11 Bugfix-Audit)
  warten weiterhin auf die Merge-Freigabe des Users.
