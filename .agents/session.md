# Session Log — Hydra-Stone

> Protocol: see `.agents/skills/session-workflow/SKILL.md`.
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
  `errors.md`, guide in `.agents/docs/ANDROID_SKILL.md`.
- ✅ **2026-10-01 — CI lint stage (PR #7, merged):** Dual-compiler
  syntax gates (gcc+clang), cppcheck, Python format gate,
  error-artifact upload. cppcheck caught a real `%u`/signed issue on
  first run.

- ✅ **2026-10-02 — Bugfix audit (PR #11, merged by user):** NEON sign
  inversion (PoC: x86 `211,388,...` vs. ARM `211,28,...`) and layer
  overflow (UB) fixed, plus 12 further findings; `test_neon_matches_scalar`
  compares NEON and scalar **in the same build** and immediately found a
  second, latent lane-offset bug. 49 tests x86 / 51 ARM. **[historical,
  superseded]** — those counts were true on that date; the suite has grown
  since. The current counts are parsed from the test binaries by
  `tools/test_count_check.sh` (see `tools/test_count_baseline.txt`), never
  written down here.
- ✅ **2026-10-02 — Web console, skills, Android APK:** see
  "Done (earlier today)" and `.agents/docs/ANDROID_SKILL.md`.

## Done (earlier today)

- ✅ Skills created + protocol files (PR #8, merged by user)
- ✅ Frontmatter v1 added (PR branch), then hardening (ASCII + quoted,
  **PR #9, merged by user at 15:46:21Z**) — main now serves valid
  skills; user invoked `/android-ndk-build` successfully → skills work
- ✅ Skill optimization after a verified end-to-end run (PR #10, merged
  by user)
- ✅ All PRs from that line are now closed: #7–#14 merged

### CI fix round (2026-10-03)

- [x] **Blocking CI failure fixed at the root.** `AssertionError: a longer
      prompt must change the output` was **not** a prompt bug: the starter
      model had `B[0] == 0`, so the decoder's `acc[0] = A[0]*token +
      B[0]*state[0]` dropped the state term entirely and prefill could not
      possibly change the output. `tools/make_model.py` now guarantees
      `A[0] != 0` and `B[0] != 0` by construction and asserts it. Verified:
      seed 5 → `[11, 33, 109]` vs `--prompt 1,5` → `[13, 41, 137]`.
- [x] **The starter model is no longer a degenerate cycle.** It emitted
      `471, 42, 471, 42` forever; it now emits
      `471, 386, 385, 386, …` - the recurrence actually contributes.
- [x] **`bench_run` initialised its out-parameter on every path** (real
      uninitialised read, only masked by statement order in the caller).
- [x] **`sscanf` → `strtoul`** for `/proc/self/statm`; a malformed line used to
      report a plausible `0 KiB` instead of "unknown".
- [x] **`bugprone-easily-swappable-parameters`**: the three mutually-convertible
      scalars (`start_token`, `steps`, `mode`) are now a `BenchRequest` struct
      passed by pointer. An enum was tried first and did **not** work — C enums
      are still implicitly convertible to every integer type, so the check
      still fired. Verified with CI's *exact* `--checks` list and file set,
      which is what the first (falsely green) local run had omitted.
- [x] clang-tidy + clang Static Analyzer: **0 findings**.
- [x] ASan+UBSan clean on the suite (71) and on the CLI bench path.
- [x] Cross-platform determinism re-verified on the **new** model across three
      targets: x86-64 host, ARM64/NEON under qemu, and the x86_64 emulator all
      emit `[471, 386, 385, 386, 385, 386, 385, 386]`.

## Round 2026-10-26 (round 6: CI red fix, local Kotlin type-check, .agents/ reorg — owner request)

**Owner request (verbatim):** "CI ist rot! :(" — fix it; "Teste alles lokal
mit Python"; "Immer Internet benutzen" + install missing tools (pip/npm);
move all agent-specific docs into a new `.agents/` folder (README.md and
AGENTS.md stay at root); add Rules etc. there; improve the agent files;
create a user-rules file (`.agents/user.md`); read `.agents/` before any
code, update it at the end of the round; adjust Environment.md.

### Root causes of the red CI (main f79ff96, all four workflows)

1. **CI job 1c (cppcheck):** `uninitvar` ×2 — `tests/test_engine.c:2790-91`,
   the v3 report roundtrip test's `hx[16]`/`hy[16]` were passed to
   `hydra_engine_report()` before cppcheck could prove a per-iteration
   write. Fix: zero-init `= {0}` (codebase convention for
   incrementally-filled buffers).
2. **Lint & Security "Android lint" + Release "Android APK" + CI Android
   jobs:** `HydraApp.kt:1011` — round-5 `SettingSlider` took
   `valueRange: ClosedFloatingPointRange` and used `.last`/`.first`, which
   only exist on IntRange. Found via check-run annotations
   (`gh api .../check-runs/<jobId>/annotations`) because the job logs and
   artifacts are blob-blocked from the sandbox. Fix: the slider takes
   `start: Float, end: Float` + step; step count from the span; the 6 call
   sites converted.
3. **Code-scanning workflow: corrupted GitHub registration.** The workflow
   was registered 2026-10-26 with the file PATH as its name and a broken
   trigger set: it ran on EVERY push (runs #20-#22, event=push, 0 jobs,
   fails in <3s, no logs — the "zero-job" signature). The file itself is
   valid YAML (PyYAML + the repo's duplicate-key gate both clean). Fix:
   force a fresh registration by moving it to
   `.github/workflows/code-scanning-alert.yml` (same content, same
   `name: Code scanning alert handling`). After the merge, the old
   registration (path `code-scanning.yml`) is orphaned by the deletion and
   the new path registers from the known-good content.
4. **clang gate 1b: `-Wtypedef-redefinition` in hydra_model.h (C99).**
   `hydra_kv_cache_t` was typedef'd twice (forward at line 153 + repeated
   on the full definition at line 287) — legal in C11, a warning under
   clang `-std=c99` → `-Werror` red. GCC is silent (its diagnosis sits
   behind `-Wpedantic`), which is why every local gcc gate stayed green.
   Introduced in round 4, merged in PR #57; the "lint" job of ci.yml was
   red on main since the merge (timeline: 12:11 green, 13:37+ red).
   Fix: full definition is now a plain `struct hydra_kv_cache { … };`
   (the name stays bound by the forward declaration). Diagnosed by
   reproducing gate 1b locally with `zig cc -target x86_64-linux-gnu`
   (zig bundles a Clang frontend — new local capability, see
   Environment.md round-6 item).
5. **Kotlin `weight` — a false alarm, corrected after CI's K2 error.**
   The local stub check flagged `Modifier.weight(1f)` (×5) as
   unresolved; the first "fix" added
   `import androidx.compose.foundation.layout.weight`. CI's K2 compiler
   rejected the import: `Cannot access 'val RowColumnParentData?.weight:
   Float': it is internal in file`. Real API (Compose source,
   RowColumnImpl.kt): `weight` is a member extension of RowScope/
   ColumnScope — resolvable inside the scope lambdas, import forbidden.
   The import was removed (the round-5 code was right on this point);
   the stub now declares `weight` as scope members, faithfully rejecting
   the import form. Full story in `.agents/errors.md`.

### The local Kotlin type-check (the capability this round added)

No Android SDK is possible in the sandbox (Maven blocked), but the Kotlin
COMPILER is on the npm registry:
`npm install kotlin-compiler@2.4.21 javajre-linux-64@17.0.8` (kotlinc-jvm
2.4.21 on JRE 17.0.8, matching the CI Temurin 17 major). Built a faithful
stub classpath (`tools/kotlin_stub_check/stubs/`, 40 files: android.*/
androidx.*/compose/org.json + R generated from the real strings.xml) and a
runner: `bash tools/kotlin_stub_check/kotlin_stub_check.sh` → compiles all
10 app .kt files in ~20s. This caught root cause #2 locally, before CI. Verified: **0 errors** on the fixed tree
(only 3 warnings, 2 pre-existing redundant-`else`, 1 stub artifact).
Stub-fidelity rules learned (see `.agents/errors.md`): verify API
signatures on the web before stubbing (Compose `items` key lambda is
single-parameter `(item: T) -> Any` — the two-parameter one is
`itemsIndexed`; `weight` is a scope MEMBER, not a top-level
extension); one package per stub file; fun-interface for Java SAMs;
JVM signature clashes when stubbing var+fun pairs. A stub error means
app code OR stub is wrong — the real API is the tie-breaker (the
`weight` episode proved it in both directions).

### .agents/ reorg (owner directive)

Moved (git mv): rules.md, errors.md, session.md, Environment.md, CLAUDE.md
→ `.agents/`; docs/{ANDROID-AUDIT,ANDROID_SKILL,RESEARCH,
HYDRA2-RESEARCH,gpu-feasibility,pr-gpu-feasibility}.md → `.agents/docs/`;
`.agent_memory/` → `.agents/docs/{TECHNICAL-AUDIT,RESEARCH-LOG}.md`
(folder deleted). Root keeps README.md + AGENTS.md; `docs/` keeps the
product specs (FORMAT.md, FORMAT-V2.md, GGUF-IMPORT.md, ARCHITECTURE.md).
All path references updated repo-wide (44 files, incl. CI comments, tools,
code comments, AGENTS.md pointer table); `tools/docs_claim_check.sh` FILES
array extended with `.agents/docs/*.md` (gate still passes). New:
`.agents/user.md` (owner's standing directives, dated, 21 entries) and
`.agents/README.md` (index + maintenance rules). Historical CHANGELOG
entries keep their dated old paths on purpose.

### Verification (all local, real outputs)

- `make test-run`: 174/0 + 86/0 + 7/0 + 73/0 (unchanged).
- cppcheck (venv, --enable=all -Isrc -Itests): 0 issues.
- `tools/kotlin_stub_check/kotlin_stub_check.sh`: OK (0 errors).
- `bash tools/docs_claim_check.sh`: all banned claims absent.
- `bash tools/workflow_yaml_check.sh`: 6 files parsed, negative control ok.
- `python3 tools/string_format_check.py`: ok (39 checked).
- PyYAML full parse of all 5 workflows: clean (installed via pip — the
  stdlib-only fallback in workflow_yaml_check.sh cannot see syntax
  errors; it passed, so the rename risk is covered by both).
- **clang gate-1b replica (local, new):** `zig cc -target x86_64-linux-gnu
  -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra
  -Werror` on all 5 CI files → reproduced the #4 failure before the fix,
  5/5 clean after.
- **NEON path after the header change:** `zig cc -target aarch64-linux-gnu`
  full compile of both src files → rc 0.

**Files:** tests/test_engine.c, include/hydra_model.h, android/app/src/
main/java/dev/hydrastone/ui/HydraApp.kt (weight import removed — false
alarm), tools/kotlin_stub_check/ (new; stubs corrected for scope-member
weight), .agents/ (new folder: 5 moved files + 8 moved docs + user.md +
README.md), .github/workflows/code-scanning.yml →
code-scanning-alert.yml, 44 reference updates.

**Open (superseded by follow-up 2 below):** PR #59 open. After merge:
confirm no zero-job push runs and all four workflows green on main —
see follow-up 2 for what the forensics actually showed.

### Round-6 follow-up (after the first PR #59 CI round, 2026-10-26)

Two more reds surfaced in the first CI round of PR #59 and were fixed
the same day (commit after 303b5c8):

1. **Android lint job: gate crash + 2 real findings.** With the Kotlin
   compile finally fixed, the job reached the lint analysis for the
   first time in the repo's history. The annotation step crashed parsing
   the `.txt` report as XML (see errors.md) — hiding the findings.
   Real findings identified by resource audit: `models_empty_title` /
   `models_empty_body` unused (UnusedResources) — the models-tab empty
   state had never been implemented. Fix: implemented the empty state in
   `ModelsTab` (uses both strings, `model_none` placeholder removed from
   strings.xml, R stub regenerated: 105 strings + 6 plurals) and fixed
   the gate to parse the XML report with text/gradle-died fallbacks and
   best-effort annotation (R25: the gate must always report its
   findings).
2. **`Modifier.weight` import removed** (see root cause 5 in the list
   above — K2 rejects the import; real API = scope member, no import).
3. **clang gate 1b fixed** (hydra_model.h typedef redefinition, root
   cause 4 above) — verified in CI: the `lint` job of ci.yml turned
   GREEN on the PR after the fix.

Status after the second CI round: ci.yml `lint` PASS, all fast checks
PASS; Android lint was re-running with the fixed gate — its
annotations are the source of truth for any remaining findings.

### Round-6 follow-up 2 (third CI round, 2026-10-26 evening)

1. **The fixed gate worked — it published its first findings** (run
   40339750614): exactly two, both fixed in **3354a9f**:
   `AutoboxingStateCreation` at `HydraApp.kt:994` (`mutableStateOf`
   with a Float → `mutableFloatStateOf`, stub gained the matching
   top-level function) and `PluralsCandidate` at `strings.xml:143`
   (`log_context_applied` "%d tokens" → `<plurals>` with one/other;
   call site MainActivity.kt:658 → `R.plurals.…`; R stub
   regenerated: 104 strings + 7 plurals). Stub check 0 errors,
   string-format check ok.
2. **Code-scanning forensics finished:** the rename did NOT clear the
   old registration — BOTH (`code-scanning.yml` id 396960786,
   `code-scanning-alert.yml` id 397125007) are still `active` and both
   carry the file PATH as name (unique to these two; all other
   workflows show their real `name:`). The new name produced the same
   phantom zero-job runs, auto-retried by GitHub every ~13 min
   (13:59/14:12/14:26 — not our pushes). The loop stopped by itself
   after 14:26; nothing in the following 45+ min. `DELETE
   .../actions/workflows/{id}` is 404 (token scope) — if it wakes up,
   the owner must delete the stale workflow in the GitHub web UI.
   **Not blocking the PR** (push-event runs never appear in `gh pr
   checks`).
3. **Sandbox reset incident:** the local branch pointer came back at
   f79ff96 while all four commits lived only on the remote; worktree
   intact. Recovered with `git fetch` + mixed `git reset
   origin/arena/ed8432d1-bestai` (pointer + index only, worktree
   untouched → clean). New rule (errors.md): after every push-ending
   turn, verify `git log --oneline -1` == last pushed sha at the start
   of the next turn. `/tmp` also wiped — kotlinc reinstalled from the
   Environment.md recipe (~1 min).

**Open (superseded by follow-up 3 below):** PR #59 open, branch head
3354a9f.

### Round-6 follow-up 3 (fourth CI round + root-cause correction, 2026-10-26 evening)

1. **ROOT CAUSE CORRECTION — the "corrupted GitHub registration" theory
   was WRONG.** The phantom zero-job runs (0 jobs, same-second failure,
   run named by the file PATH) were **compile failures of any workflow
   file naming `on: code_scanning_alert`**: that event is a Webhook
   event, NOT a GitHub Actions workflow trigger event (not in the
   current docs trigger list — page runs `check_suite` → `create` with
   no such section — and actionlint rejects it: `unknown Webhook
   event`). Proof by causality: the phantom always named the file
   containing the bad event, and followed it across three commits
   (`code-scanning.yml` 13:37-13:53 → `code-scanning-alert.yml`
   13:59-15:11 → `lint.yml` 15:13, the latter after I moved the trigger
   in — where it again produced the zero-job failure AND the Lint&Sec
   PR run went missing). The `name` = path display and the 14:12/14:26
   "auto-retries" were red herrings (no `name:` to show on a
   compile-failed run; more dispatch attempts of the same broken file).
   PyYAML/the duplicate-key gate cannot see this: they validate YAML,
   not the Actions event schema.
2. **Action:** (a) deleted `.github/workflows/code-scanning-alert.yml`
   (1b0ace5); (b) reverted the `code_scanning_alert` trigger + alert
   job out of lint.yml; (c) NEW GATE 1i `tools/workflow_lint_check.sh`
   = actionlint (npm WASM build, `/tmp/al` self-recreating, one fresh
   node process per file — the WASM runtime crashes intermittently on
   multi-file processes) over every workflow file + in-gate negative
   control `tools/fixtures/workflow_lint_bad.yml` (the invalid trigger
   event itself; R25). Wired into ci.yml after gate 1h. Code-scanning
   REACTION stays native: CodeQL/Semgrep jobs report on every PR
   (annotations + check results) and alerts close themselves as fixed
   when the next default-branch analysis no longer reports them.
3. **Android lint round 3 (run 40346890465, 3354a9f):** both original
   findings cleared; the plurals conversion introduced ONE new finding
   — `MainActivity.kt:658 ResourceType: Expected resource of type
   string`: `Context.getString` does not accept `R.plurals.*`
   (`@StringRes` vs `@PluralsRes` is enforced by LINT, not kotlinc —
   the stub check cannot express resource annotations and stayed
   green). Fix: `getQuantityString(R.plurals.log_context_applied,
   new.contextSize, new.contextSize)`; stub `A_content.kt` gained
   `getQuantityString`; stub-check README gained a KNOWN-LIMITATIONS
   section (ResourceType / AutoboxingStateCreation / PluralsCandidate /
   UnusedResources / Compose transforms are CI-lint-only classes).

**Open (superseded by follow-up 4 below):** PR #59 open, fix commit
7c534b2 pending.

### Round-6 follow-up 4 (merge/revert incident + stage-2 API fix, 2026-10-26 evening — new session branch `arena/72470567-bestai`)

1. **Incident:** PR #59 was merged by the owner at 15:15:21 at head
   **1b0ace5** (the broken state — bad `code_scanning_alert` trigger in
   lint.yml; the fix commit 7c534b2 landed 3.5 min later, after the
   merge). Main went red immediately (zero-job compile-failure run of
   lint.yml). 16 s after the merge the owner opened **PR #60 (full
   revert of #59)** out of the red; the owner then CLOSED #60 and asked
   for "everything correct" to land (PR #62 was opened from
   `arena/ed8432d1-bestai` carrying exactly 7c534b2).
2. **PR #62 CI exposed stage 2 of the plurals fix:** all jobs green
   except Android lint — `MainActivity.kt:658:25 Unresolved reference
   'getQuantityString'`: **`Context` has no `getQuantityString`**, it is
   a `Resources` method (API 1, developer.android.com pattern
   `context.resources.getQuantityString(R.plurals.…, count, count)`).
   The stage-1 fix had added a fabricated `Context.getQuantityString`
   to the STUB (violating the "never guess stub members" rule) — the
   stub check passed, the real compiler failed. Final fix on this
   branch: `resources.getQuantityString(R.plurals.log_context_applied,
   new.contextSize, new.contextSize)` (the pattern the codebase
   already used at 395/536/796), fabricated Context stub member
   removed, errors.md entry expanded with the stage-2 lesson.
3. **State:** this session branch = main (0ad01dd) + 7c534b2 + the
   stage-2 fix. PR #62 (old branch) is red and should be CLOSED; the
   green vehicle is the new PR from this branch. Stale workflow
   registrations (396960786/397125007) remain listed but inert (no file
   at the path = no dispatch); PAT cannot delete them (404), owner can
   via the web UI.
## Follow-up after the merge (2026-10-26)

- [x] **Root cause of ALL zero-job phantom runs (2026-10-25 → 26):** `on: code_scanning_alert` is a Webhook event, NOT a workflow trigger event. GitHub's workflow compiler rejects a file that names it — the run is created with zero jobs, fails in the same second, and the run NAME is the file path. PyYAML (gate 1h) could never see this (valid YAML; the event name is a string).
- [x] **Fixed in gate 1i (new):** `actionlint` over every workflow file (npm, /tmp, ephemeral re-setup) + in-gate negative control. `bash tools/workflow_lint_check.sh` locally. ci.yml + lint.yml no longer name `code_scanning_alert`; auto-close policy unchanged (the alert workflow remains, its trigger is `check_suite`).
- [x] **Cherry-pick of 7c534b2 + the stage-2 fix + the root-cause docs shipped as PR #64** (arena/72470567-bestai): PR checks 15 pass / 4 skipped / 0 fail; Android lint green; both branch workflows success; zero phantom runs.
- [x] **PR #62 opened** (arena/ed8432d1-bestai, head 7c534b2): same content minus the stage-2 correction — its CI red = the stage-2 error itself, now fixed in #64. To be closed as superseded (owner merges).
- [x] errors.md + session.md + Environment.md updated; gate sweep green (workflow-yaml, actionlint + neg control, docs-claim, string-format, kotlin stubs, shellcheck); PR body facts-only.

**Status (2026-10-26 15:33):** PR #64 MERGED → main 19df869 (the PR tier is now fully green for the first time: build/test/neon + every lint gate). Remaining item: close PR #62 as superseded — done in round 7.

## Round 2026-10-26 (round 7: rule adaptation + .agents/ skills + all-.md audit)

**Session start (R33):** 2026-10-26 (branch `arena/e28fb029-bestai`, base main 19df869 = merged PR #64, all checks green). Read before touching code: AGENTS.md (full) → session.md (current round) → errors.md (top 3 entries) → rules.md (R1–R33, §6–§8) → user.md (5 owner rules + dated directives) → Environment.md (Arena section) → engine-ci-verify + ci-triage SKILL.md. Web research for the R33 root-cause question (2026 sources; only-enforcement-guarantees + context-dilution findings recorded under the R33 rule and in errors.md).

**Owner directives (2026-10-26, verbatim intent):**
1. „Passe deine Regeln an, du liest nie diese vorher“ — the read-`.agents/`-before-work rule kept being SKIPPED; adapt the rules so it reliably happens, and investigate why (2026 web research on agent context engineering).
2. „in .agents Ordner bitte mit skills füllen, lese dafür die, die auch wirklich in Claude Ordner sind“ — fill `.agents/` with skills; base them on the skills that really exist in `.claude/skills/`.
3. „Verbessere alle .MD Dateien suche alle .MD Dateien, in jedem Ordner“ — find and improve EVERY .md file in every folder.
4. „PR #62 was ist damit?“ — PR #62 is the red leftover of the #59-merge incident, superseded by the now-merged #64 → close it.

**Context:** PR #64 merged green at 15:33 (main 19df869 = stage-2 plurals fix + root-cause fix + gate 1i). PR #62 still OPEN (superseded). This round fixes the PROCESS (rules/skills/docs), not engine code.

**Diagnosis of the skipped reads (2026 sources, see R33):** (a) prose-only rules are followed probabilistically — "instructions raise the probability, only enforcement guarantees it" (prompt-architects, 2026); (b) context dilution — the root instruction file is loaded on EVERY request and a 2400-line session.md buries the protocol; (c) arXiv May-2026 study (1650 Claude Code sessions): compliance odds drop ~5.6% per additional function in the instruction set, file position/size had no effect → the fix is a verifiable marker + a gate, plus slimming.

**Plan:**
- [x] R33 added to rules.md (mandatory session-start read order + `**Session start (R33):**` marker in the current round section BEFORE the first code edit) + CI gate `tools/session_start_check.sh` (deterministic check of session.md + in-gate negative control per R25), wired into ci.yml as gate 1j.
- [x] `.agents/skills/` created as the CANONICAL skills home (2026 Agent Skills spec: name == dir, quoted description, what+when): the 3 skills moved out of `.claude/skills/` (git mv), `.claude/skills` is now a symlink to `../.agents/skills` (Claude discovery keeps working; R30: all agent docs live in `.agents/`).
- [x] 4 NEW skills (written from verified repo knowledge only, no invention): `ci-triage` (annotation-reading, zero-job signature, local gate replicas), `kotlin-stub-check` (setup/run + the stub-fidelity rules learned in rounds 5–6), `android-lint-gate` (annotation reading, lint-only classes ResourceType/PluralsCandidate/AutoboxingStateCreation/UnusedResources, correct APIs), `repo-docs-audit` (this round's audit made repeatable).
- [x] session.md slimmed: everything before round 6 moved verbatim to `.agents/docs/SESSION-ARCHIVE.md` (context-dilution fix; nothing deleted).
- [ ] All-.md audit (28 files listed in Workspace): stale paths, dead refs (deleted code-scanning workflow, old PR states), banned claims, R31 language.
- [ ] errors.md entry (skipped-reads → R33 + this round's findings); user.md entry for the 4 directives.
- [ ] Close PR #62 (superseded by merged #64; #62 CI red = the stage-2 error #64 fixed).
- [ ] Gates green (incl. new 1j), commit, push, PR (facts-only; owner merges).

**Progress:**
- [x] Session start: AGENTS.md + .agents/{rules,session,errors,user}.md read; 28 .md files listed; 2026 web research done (AGENTS.md budgets/dilution; enforcement-over-prose; Agent Skills spec) — findings recorded in R33 + errors.md + the ci-triage/repo-docs-audit skills.
- [x] R33 + gate 1j (tools/session_start_check.sh, wired into ci.yml after 1i; local run: ok, negative control proven).
- [x] `.agents/skills/`: 3 moved (session-workflow, engine-ci-verify, android-ndk-build) + 4 new (ci-triage, kotlin-stub-check, android-lint-gate, repo-docs-audit); `.claude/skills` = symlink.
- [x] session.md slimmed to current rounds (archive: .agents/docs/SESSION-ARCHIVE.md).
- [x] all-.md audit + fixes (28→33 files inventoried; 0 broken links; fixed: AGENTS.md §8 dead code-scanning.yml ref (rewritten to verified reality), FORMAT-V2 ↔ HYDRA2-RESEARCH cross-links, root README tree + CI-tier note + skill pointer, .agents/README index (skills table, R33, archive), android/README dual-engine reality (file tree, libs, run flow, tabs), CHANGELOG backfill (rounds 5/6/6-follow-up/7 were missing), RESEARCH-LOG §7 correction (event-triggered workflow could never run — deleted same day), session-workflow skill (R33 step + format template), android-ndk-build description (8 pitfalls), rules/README/AGENTS "32 rules" → 33)
- [x] errors/user docs (errors.md: R33 root-cause entry + the archive-split incident; user.md: directives 22–25)
- [x] PR #62 closed (superseded; factual comment records why: its CI red = the stage-2 API error fixed by merged PR #64)
- [x] commit + push + PR: **PR #65** (ec344a9). CI: **15 pass / 4 skipped / 0 fail** (run 40353536462 + 40353536695) — incl. the NEW gate 1j (session-start marker) green on CI, docs-claim green with the archive exclusion + negative control, Android lint green. Main-only tier jobs correctly skipped on the PR.

**Awaiting owner:** merge PR #65. Post-merge: main carries R33 + gate 1j; any session that skips the read protocol will turn the next push red.


## Round 2026-10-26 (round 8: full transformer attention (v4), .hydra2 sparse-MoE, Android sampler, CLAUDE.md fix — owner prompt)

**Session start (R33):** 2026-10-26 (branch `arena/fe34926d-bestai`, base main 3d13d5a = merged PR #65, all checks green). Read before touching code: AGENTS.md (full) → session.md (current round) → errors.md (top 3) → rules.md (R1–R33) → user.md (25 directives) → Environment.md (Arena section) → engine-ci-verify + ci-triage + repo-docs-audit SKILL.md → docs/FORMAT-V2.md (full), docs/FORMAT.md (v3 section), docs/ARCHITECTURE.md (full). PLUS the owner's directive for this round: read ALL .md files first — done (33 files, all folders; the 4 product specs, all 7 skills, all .agents files, both READMEs, CHANGELOG).

**Owner directives (2026-10-26 evening, verbatim intent):** "session.md kann bleiben" (do NOT optimize/rewrite session.md; it stays committed and readable by everyone — verify it is not git-ignored) + "habe ein prompt erstellen lassen" (the pasted Principal-Architect prompt is the work plan) + "lese alle dateien, alle .md dateien, als erstes, und dann bitte optimieren (außer session.md …)" + "weiter machen, suche auch im Web wenn nötig" + "Bitte intelligent" + "Finde Fehler und toten Code" + "lade über python weitere Linter und so herunter" + "KEINE Stubs oder Fake". No native todo tool exists in this environment — the plan below is the todo list (session.md precedent).

**Prompt audit, corrected against live main 3d13d5a (the prompt was written against a pre-round-4 state; R26 — verify before acting):**
- Claim "5 of 7 roadmap items checked; [ ] Full transformer forward pass, primitives NOT wired into an end-to-end layered forward pass" — **STALE.** Round 4 (merged via PR #57 → main) already shipped `hydra_engine_step_tf()`: per-layer ternary SwiGLU blocks + RMSNorm + RoPE + KV sink/append, format v3, tested (`test_tf_*`), roadmap item already `[x]`. What is genuinely missing for a FULL transformer: **attention** — no Q/K/V projections, no causal scaled dot-product attention.
- Claim ".hydra2 sparse-MoE: zero implementation" — **TRUE** (FORMAT-V2.md: "No part is implemented").
- Claim "hydra_jni.c does not expose hydra_sample_token / KV state to Android" — **TRUE** (HydraEngine.kt ignores sampling params; documented on the interface).
- Claim ".claude/skills symlink breaks on Windows; CLAUDE.md hidden in .agents/" — **TRUE** (web-verified 2026-10-26: Claude Code v2.1.277+ reads AGENTS.md only as a FALLBACK when no CLAUDE.md/.claude/CLAUDE.md/CLAUDE.local.md exists above the cwd; a committed symlink checks out as a one-line text file on `core.symlinks=false`; Claude's Edit/Write refuse symlinked paths; the import pattern `CLAUDE.md` containing `@AGENTS.md` is the cross-platform fix — sources recorded in RESEARCH-LOG §10).
- Invariant kept: the v1/v2 starter baseline `[471, 386, 385, 386, 385, 386, 385, 386]` (start_token=42) stays 100% intact — all engine work is ADDITIVE (new format versions / new functions), never a rewrite of the legacy path (user.md #11).

**Plan (milestones; order = dependency, each verified locally before the next — zero stubs, zero fakes):**
- [x] M-A: R33 session start — all 33 .md files read; round 8 section + marker; prompt audit corrected above.
- [x] M-B: session.md visibility — verified tracked (23 .agents files in git, `git check-ignore` clean); fix the STALE .gitignore comment (names the deleted `.agent_memory/` + top-level RESEARCH.md); add `.agent_memory/` as an ignore (local scratch only — repo memory lives in `.agents/` per owner decision).
- [x] M-C (prompt M4): real root `CLAUDE.md` (`@AGENTS.md` + `@.agents/CLAUDE.md`), docs updated (AGENTS.md §5 table, .agents/README.md, CHANGELOG); Windows symlink behaviour documented honestly.
- [x] M-D (owner: linters + bugs + dead code): pip-install additional linters (pylint, bandit, pyflakes for the Python tools; existing: cppcheck/clang-tidy/shellcheck/ESLint) → run everything → fix REAL findings; dead-code hunt (declared-but-never-called functions in C; unused imports/vars in Kotlin/JS/Python).
- [x] M-E (prompt M3): expose the C sampler (temperature / min-p / repetition penalty) through `hydra_jni.c` + `HydraBridge.kt` + `HydraEngine.kt`; default greedy calls 100% unchanged; kotlin stubs updated with the new `external fun`s; `kotlin_stub_check` + `compose_scroll_check` + `jni_signature_check` (CI javac) green.
- [x] M-F (prompt M1, corrected): FULL attention on top of v3 → **format version 4** (new version value, backward compatible: v1/v2/v3 files and the baseline stream untouched): per-layer Wq/Wk/Wv ternary planes + the existing Wg/Wu/w1/scale; `hydra_transformer_forward_step()` = pre-norm RMSNorm → ternary Q/K/V (NEON/AVX2/scalar dispatch) → RoPE → KV cache (sinks + window) → causal scaled dot-product attention (deterministic fixed-point) → residual → RMSNorm → SwiGLU FFN → residual → final RMSNorm → W_out logits; CLI + engine dispatch; tests incl. KV progression, RoPE position increment, min-p sampling, hand-computed case, NEON-vs-scalar same-build parity.
- [x] M-G (prompt M2): `.hydra2` sparse-MoE container per docs/FORMAT-V2.md — header parser + BOTH-ENDS validation (R14), top-k router (softmax over selected), expert streaming loader (pread, crc32 per read), fixture generator (make_model.py or gguf_to_hydra.py), tests (`test_hydra2_*` + security refusals + routing determinism).
- [x] M-H (prompt M5): roadmap checkboxes only after M-E/M-F/M-G are green (M6 invariant: 100% implemented + automated test); `tools/test_count_baseline.txt` updated in the same commit as the tests.
  - 2026-10-26: M-B..M-G deliverables each verified present in the tree before checking (M-B: .gitignore comment + `.agent_memory/` ignore; M-C: real root `CLAUDE.md`; M-D: linter runs + fixes from the previous round; M-E: `hydra_sample*` plumbing in `hydra_jni.c`; M-F: v4 loader/engine/tests + baseline stream re-verified; M-G: `src/hydra_v2_container.c` + 118-check `hydra2-test` (ASan/UBSan clean, gcc `-fanalyzer` clean on all three C files, fixture generator, CI wiring in `ci.yml` + `lint.yml`). Baseline file: x86 174->219, arm 183->228 with the dated entry (ARM re-measure on the CI leg). `docs/FORMAT-V2.md` updated: real implementation status, K/V wire format + 16-byte alignment recorded as `[CODE]` decisions.
- [x] M-I: .md optimization pass (all files EXCEPT session.md) — findings from the full read fixed in place; docs for the new versions/sampler/CLAUDE.md; CHANGELOG entry; Environment.md round-8 section (own section only).
  - 2026-10-26: all 33 .md files scanned for round-8 staleness. Fixed in place: `docs/FORMAT-V2.md` (implementation boundary, K/V wire format + 16-B alignment as dated `[CODE]` decisions), `docs/FORMAT.md` (new "Version 4" section: layout/forward pass/constraints/honest no-v4-writer status; the 4-GiB note no longer says "not yet implemented"), `docs/ARCHITECTURE.md` (new "Version 4 forward pass" section + components table), `README.md` (v4 feature line, Android sampler clause, the `.hydra2` roadmap item split into done-container [x] + open-inference [ ] with the spec-section evidence), `android/README.md` (the "ignores sampling params" claim is false since M-E: runInferenceSampled + fixed seed + temp-0 baseline), `CHANGELOG.md` (round-8 entry), `.agents/Environment.md` (round-8 section: pip linter installs + versions + full finding triage + recreate commands), `.agents/errors.md` (two real analyzer-found bugs + the regex-insertion process error). Files verified stale-free and left untouched: user.md, rules.md, the 7 skills, the .agents/docs/* research files (their "no v2 reader" statements are historical records of their own audit dates).
- [x] M-J: full gate sweep (AGENTS.md §3 order), commit, push, PR (facts only; owner merges).
  - 2026-10-26: gate sweep per AGENTS.md §3 order — every gate green or explicitly
    SKIP/blocked-out-loud (jni_signature: no javac, CI covers; ARM execution + NDK:
    no toolchain/SDK in sandbox, CI covers; mirror-test clang step: no clang driver,
    said out loud per R23). Full results in the commit message of 75b2649 and in the
    PR #67 description. Committed as 75b2649 (40 files, +5022/−212), pushed, PR #67
    opened (facts only). Merge: owner.
