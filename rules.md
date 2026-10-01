# Working Rules — Hydra-Stone

> Standing rules for the agent working in this repository.
> Protocol details: `.claude/skills/session-workflow/SKILL.md`.
> Rules are added or changed when agreed with the user or learned from
> a mistake (cross-reference the `errors.md` entry that motivated it).

## 1. Session protocol (highest priority)

1. **R1 — Session file first.** At session start, read `session.md`,
   `errors.md`, `rules.md` BEFORE touching any code. Then write the
   plan for the current task into `session.md`.
2. **R2 — Keep the plan alive.** Update `session.md` after EVERY
   completed step. A stale plan is a broken plan. No exceptions.
3. **R3 — Log every error immediately.** Every error gets an entry in
   `errors.md` (symptom, cause, fix, prevention) at the moment it is
   hit — not at session end.
4. **R4 — Session end.** Mark done steps, move open items, verify
   `errors.md` is complete; both files go into the commit/PR.

## 2. Correctness & honesty (KEIN Fake)

5. **R5 — No fake tests.** Every test must be falsifiable. Ask for
   each test: "What code change would this catch?" Tautologies
   (`CHECK(1)`, same-input-same-output on deterministic code) are
   forbidden. (→ errors.md: tautology test)
6. **R6 — No claimed capabilities.** Never document a feature that is
   not wired up. SIMD on x86 does not exist → docs must say so.
   (→ PR #4/#5 honesty rewrites)
7. **R7 — Prove claims with execution.** "It works" requires a run:
   unit tests, CLI smoke test, emulator logcat — whatever matches the
   claim. Verify on the real target platform, not only on host.
   (→ errors.md: JNI dim:0 bug invisible on host)
8. **R8 — PoC before and after security fixes.** For every
   vulnerability: reproduce it, fix it, turn the PoC into a regression
   test. (→ errors.md: OOB read)

## 3. Build & code discipline

9. **R9 — Same source everywhere.** The engine has ONE implementation
   (`src/hydra_engine.c`); CLI, web console, and Android all wrap it.
   No forks of the core.
10. **R10 — stdout is data, stderr is logs.** Library code never
    prints to stdout. (→ errors.md: JSON pollution)
11. **R11 — Validate all length-deriving fields against real bounds**
    on load; any `offset + len` must be checked against file size, any
    count-based access against the buffer length.
12. **R12 — Pin toolchain versions** (NDK, CMake, AGP) to what is
    actually installed; AGP defaults are not a contract.
13. **R13 — Include what you use.** NDK clang is stricter than host
    gcc; a clean NDK build is part of the quality bar.

## 4. Git & delivery

14. **R14 — Never merge without explicit user approval.** PRs stay
    open until the user says merge. (Standing user instruction.)
15. **R15 — One logical change per PR**, with verification steps
    documented in the PR body.
16. **R16 — Never push destructive or unrequested operations** (force
    push, reset --hard, history rewrite, deleting user changes).

## 5. Verification loop

17. **R17 — Run the full local gate before committing** (skill:
    `engine-ci-verify`): gcc, clang, cppcheck, python gate, build,
    32 tests, smoke test.
18. **R18 — Determinism checks use platform-fixed seeds** (the LCG),
    never `rand()`, so ubuntu-gcc and macos-clang-ARM64 CI runners
    compare identical sequences across scalar and NEON paths.
19. **R19 — New platform targets require on-platform verification**
    (Android = emulator run with logcat evidence), not just a
    successful cross-compile.

## 6. When rules conflict

Safety rules (R8, R11) > correctness rules (R5–R7) > session protocol
(R1–R4) > convenience. If a user request conflicts with R14 (merge
approval), R14 wins unless the user explicitly overrides it in the
same session.
