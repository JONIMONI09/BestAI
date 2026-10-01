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

Create agent skill files (`.claude/skills/*/SKILL.md`), plus mandatory
working-protocol files: `session.md` (this file), `errors.md`,
`rules.md`.

## Plan

- [x] Skill: `android-ndk-build` (build + emulator verification + pitfalls)
- [x] Skill: `engine-ci-verify` (all local gates, mirrors CI)
- [x] Skill: `session-workflow` (mandatory session protocol)
- [x] Create `errors.md` with the full error history from this project
- [x] Create `rules.md` (standing working rules)
- [x] Commit + open PR (NOT merge — merging only on explicit user request)

## Status / Notes

- PR #7 (CI lint) is still open and intentionally unmerged — awaiting
  explicit user approval.
- Next logical steps after this: merge PR #7 on approval; optionally
  add Android build to CI.
