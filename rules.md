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

25. **R25 — A new lint gate must fail once.** Before a linter or CI gate
    counts as protection, it must be proven to fail on a deliberately
    reverted fix (negative control). A gate that never goes red is
    decoration, which is the "KEIN Fake" failure mode in the other
    direction. (→ errors.md: linter-audit entry, incl. the tool-by-tool
    table of what each one actually catches)
26. **R26 — Check research against reality before acting on it.**
    CVE IDs, tool names and API behaviour are verified on the web and in
    a local run, never taken from a description. A wrong CVE attribution
    is itself a documentation bug. (→ verified CVE-2025-2439/2445,
    CVE-2025-53630, CVE-2026-27940/33298/70638)
27. **R27 — Never leak internal detail in an error response.** Stack
    traces, absolute paths, file sizes and subprocess stderr go to the
    server log; the client gets an opaque error. (→ errors.md: server
    detail disclosure)
28. **R28 — Path checks validate the resolved target, not the name.**
    `path.resolve` is lexical and follows no symlink; use `realpath` (or
    `O_NOFOLLOW`) so a symlink inside the allowed directory cannot point
    outside it. (→ errors.md: symlink model path)

## 7. Skills (`.claude/skills/*/SKILL.md`)

29. **R29 — Every SKILL.md starts with YAML frontmatter** delimited by
    `---` lines, containing at minimum `name` and `description`. A
    skill without valid frontmatter is rejected by the UI.
    The frontmatter must be **pure ASCII** and the `description`
    must be a **quoted** scalar — non-ASCII characters (em dashes)
    in unquoted scalars get the skill rejected as "Not loaded".
    (→ errors.md: both skill-rejection entries; skill bodies may use
    non-ASCII — only the frontmatter block must be pure ASCII)
30. **R30 — Validate frontmatter before committing**: parse the YAML,
    check `name` matches the directory name, check `description` is
    present and one sentence long, assert the frontmatter block is
    ASCII-only.

31. **R31 — Every change goes through a pull request, in English.** No
    work is pushed straight to `main`: create a branch, open a PR, let the
    user merge (R14). All commit messages, PR titles, PR bodies, code
    comments, documentation (`README.md`, `docs/`, `errors.md`,
    `session.md`) and in-app user-facing strings are written **in
    English**, so that every reader can understand them. Existing German
    prose in older log messages may stay (it is engine stderr, not
    documentation), but nothing new is written in German. If the user
    writes to me in German, the *conversation* stays German, the
    *artefacts* stay English.
32. **R32 — Measure before claiming.** A performance statement, a speedup,
    a memory claim or a device number must come from a command that was
    actually executed, and the report must say on which machine it ran. If
    the measurement cannot be taken in the current environment (no device,
    no emulator), the report says "blocked, not measured" - never an
    estimate dressed as a result.

## 8. When rules conflict

Safety rules (R8, R14, R25, R27) > correctness rules (R5–R11) > session
protocol (R1–R4) > documentation rules (R31) > convenience. If a user request conflicts with R17
(merge approval) or R20 (no checkout), the user's standing rule wins
unless they explicitly override it in the same session.
