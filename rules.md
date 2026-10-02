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
   test. (→ errors.md: OOB read, layer-overflow PoC)
9. **R9 — Compare parallel implementations against *each other*.**
   Checking the SIMD path and the scalar path against fixed expectations
   cannot detect a systematic error shared by neither; only a same-build
   A/B comparison can. (→ errors.md: NEON sign inversion, NEON lane
   offsets — both shipped in a version documented as "bit-identical")
10. **R10 — A changed expected value is a decision, not a chore.**
    When a fix legitimately changes a test expectation, the old value
    must be justified in a comment as a *symptom* of the bug, never
    silently overwritten. (→ errors.md: `labs()` sign loss)
11. **R11 — Review numerical edge cases explicitly:** signed overflow,
    NaN/Inf, negative inputs, and values just outside every clamp.
    IEEE-754 makes `NaN <= 0` false — safety gates must reject
    non-finite input explicitly.

## 3. Build & code discipline

12. **R12 — Same source everywhere.** The engine has ONE implementation
    (`src/hydra_engine.c`); CLI, web console, and Android all wrap it.
    No forks of the core.
13. **R13 — stdout is data, stderr is logs.** Library code never
    prints to stdout. (→ errors.md: JSON pollution)
14. **R14 — Validate all length-deriving fields against real bounds**
    on load; any `offset + len` must be checked against file size, any
    count-based access against the buffer length, and any field that is
    *multiplied* into an accumulator bound must be capped so the
    arithmetic cannot overflow.
15. **R15 — Pin toolchain versions** (NDK, CMake, AGP) to what is
    actually installed; AGP defaults are not a contract.
16. **R16 — Include what you use.** NDK clang is stricter than host
    gcc; a clean NDK build is part of the quality bar.

## 4. Git & delivery

17. **R17 — Never merge without explicit user approval.** PRs stay
    open until the user says merge. (Standing user instruction.)
18. **R18 — One logical change per PR**, with verification steps
    documented in the PR body.
19. **R19 — Never push destructive or unrequested operations** (force
    push, reset --hard, history rewrite, deleting user changes).
20. **R20 — NO git checkout. The user does checkouts themselves.**
    The agent never runs `git checkout` (branch switches, detached
    HEAD). Work on whatever branch is currently checked out in the
    workspace, or commit on the current branch. (Standing user
    instruction, 2026-10-01.)

## 5. Verification loop

21. **R21 — Run the full local gate before committing** (skill:
    `engine-ci-verify`): gcc, clang, cppcheck, python gate, build,
    49 tests, smoke test.
22. **R22 — Determinism checks use platform-fixed seeds** (the LCG),
    never `rand()`, so ubuntu-gcc and macos-clang-ARM64 CI runners
    compare identical sequences across scalar and NEON paths.
23. **R23 — New platform targets require on-platform verification**
    (Android = emulator run with logcat evidence), not just a
    successful cross-compile.
24. **R24 — Cross-platform claims need cross-platform proof.** When the
    docs say two platforms behave identically, actually run both (e.g.
    `aarch64-linux-gnu-gcc-12 -static` under `qemu-aarch64-static`
    for ARM without a device) and compare the actual outputs.

## 6. Skills (`.claude/skills/*/SKILL.md`)

25. **R25 — Every SKILL.md starts with YAML frontmatter** delimited by
    `---` lines, containing at minimum `name` and `description`. A
    skill without valid frontmatter is rejected by the UI.
    The frontmatter must be **pure ASCII** and the `description`
    must be a **quoted** scalar — non-ASCII characters (em dashes)
    in unquoted scalars get the skill rejected as "Not loaded".
    (→ errors.md: both skill-rejection entries)
26. **R26 — Validate frontmatter before committing**: parse the YAML,
    check `name` matches the directory name, check `description` is
    present and one sentence long, assert the frontmatter block is
    ASCII-only.

## 7. When rules conflict

Safety rules (R8, R14) > correctness rules (R5–R11) > session protocol
(R1–R4) > convenience. If a user request conflicts with R17 (merge
approval) or R20 (no checkout), the user's standing rule wins unless
they explicitly override it in the same session.
