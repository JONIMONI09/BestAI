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

Root cause found: **PR #9 is still OPEN** — the ASCII frontmatter fix
never reached `main`. `main` still carries the em-dash descriptions
from PR #8, which is exactly what the Skills UI rejects.

## Plan

- [x] Diagnose: main already has frontmatter (PR #8 merged), so UI
      rejects the parsing itself, not the name match
- [x] Rewrite all 3 frontmatters: ASCII-only, quoted description
- [x] Extend validator: assert frontmatter block is ASCII-only
- [x] Update rules R21/R22 (ASCII + quoted description mandatory)
- [x] Log second error entry in `errors.md`
- [x] Push fix branch WITHOUT checkout (git push origin HEAD:refs/heads/…)
- [x] Open new PR #9 targeting main
- [x] Verify PR #9 merge status → OPEN, not merged (user decision per R14)
- [x] Verify origin/main still has the em-dash descriptions (confirmed)
- [ ] **BLOCKED on user:** merge PR #9, then reload the Skills UI
- [ ] If still failing after merge: user provides the FULL untruncated
      error text from the UI dialog

## Status / Notes

- Merge status verified 2026-10-01: PR #9 `state=OPEN, mergedAt=null`.
  `git show origin/main:.claude/skills/*/SKILL.md` shows the OLD
  em-dash descriptions on main — the UI is rejecting exactly those.
- R17 (no checkout) respected throughout: fix branch was created via
  `git push origin HEAD:refs/heads/fix-skill-frontmatter`.
- PR #7 (CI lint) also still open, awaiting user approval.
