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

## Current task

Fix agent skills (rejected by UI — missing YAML frontmatter) and add
the user's new standing rule: NO git checkout by the agent.

## Plan

- [x] Add YAML frontmatter (`name`, `description`) to all 3 SKILL.md files
- [x] Validate frontmatter (YAML parse, name == dir name, description present)
- [x] New rule R17 in `rules.md`: NO git checkout — user does checkouts
- [x] Renumber subsequent rules, add skills rules R21/R22
- [x] Log the frontmatter error in `errors.md`
- [x] Update `session.md`, commit + push to existing PR branch (NO checkout)

## Status / Notes

- PR #8 (skills + protocol) updated in place with the frontmatter fix;
  still open, awaiting explicit user approval.
- PR #7 (CI lint) also still open, awaiting user approval.
- Standing rule now: agent works on the currently checked-out branch;
  never runs `git checkout`.
