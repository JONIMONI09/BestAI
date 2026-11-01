# RESEARCH.md — permanent research record (in-repo on purpose)

> **This file lives in the repository by owner decision (2026-10-26).** It is
> the long-term home for investigations that must survive across sessions:
> crash root causes, CI architecture, research with follow-up obligations.
>
> **Keep it current (AGENTS.md §5):** every non-trivial investigation appends
> one section here before the session ends — date, question, method
> (commands / links), finding, resolution, pointer to the permanent artifact
> (guard, test, doc). Do not turn this into a changelog — `CHANGELOG.md` is
> the user-facing log; this is the evidence trail.

## Index

| Date | Topic | Outcome |
|---|---|---|
| 2026-10-26 | Compose chat-tab crash (infinite max height) | fixed + regression guard = CI gate |
| 2026-10-26 | CI speed (measured bottleneck, not the obvious one) | Android toolchain caches shipped |
| 2026-10-26 | Issue auto-close from CI (2026 patterns) | `tools/issue_verify.py` + workflow shipped |
| 2026-10-26 | Code-scanning API access from an Actions token | blocked; workaround channels documented |
| 2026-10-26 | Sandbox tool inventory + install channels | measured; policy in AGENTS.md §6 |
| 2026-10-26 | GitHub workflow compiler vs PyYAML (duplicate keys) | zero-job red run; gate 1h shipped |
| 2026-10-26 | Auto-close target: issues → code-scanning alerts | issue-verify removed; auto-close is native GitHub behaviour — an event-triggered workflow cannot exist (see §7 correction) |
| 2026-10-26 | Local lint stack from PyPI wheels (no apt needed) | 6 real pre-merge defects caught; path in .agents/Environment.md |
| 2026-10-26 | Local/CI syntax drift: the five-file lesson (PR #55 first run) | ci_syntax_mirror_test.sh gate shipped |
| 2026-10-26 | Darwin feature-test inversion: _POSIX_C_SOURCE hides, not enables | control-based string-format gate; release.yml green |
| 2026-10-31 | CI fixture-list drift: 216 checks green locally, red on main | tools/make_moe_fixtures.sh single source; main 9/9 (§11) |
| 2026-11-01 | Newer distro cppcheck flags R16 test allocations (main red post-#79) | checked allocations via kv_set_alloc/kv_set_free; PR #80 (§12) |
| 2026-11-01 | Runner image drop: gcc-12-aarch64-linux-gnu gone from apt sources | unversioned gcc-aarch64-linux-gnu in ci.yml + release.yml (§13) |

---

## 1. Compose chat-tab crash (2026-10-26) — RESOLVED

**Question:** user crash on Android 15 (arm64, API 35), Hydra-Stone 1.0.25/1.0.26,
every chat-tab composition: `IllegalStateException: … measured with an infinite
max height and Compose doesn't support this` inside
`Scroll.kt → CheckScrollableContainerConstraints → onRemeasure →
RowColumnMeasurePolicy.kt:119 → Column.kt:208 → BoxWithConstraints.kt:67/70 →
LazyColumn measure` (foundation **1.7.3** via Compose BOM 2024.09.03).

**Method:** fetched the foundation v1.7.3 sources from the JetBrains
`compose-multiplatform-core` mirror (tags v1.7.0–v1.7.3; identical line maps)
and mapped every crash frame 1:1 to the code:
`CheckScrollableContainerConstraints.kt:35` (the throwing check),
`Scroll.kt:407/416` (verticalScroll passes `maxHeight = 1e9` into a parent with
infinite max constraint), `Size.kt:699/838` (constraint propagation),
`RowColumnMeasurePolicy.kt:119` (Column child measurement), `Column.kt:208`,
`BoxWithConstraints.kt:67/70` (infinite constraint passthrough).

**Root cause (PR #50 regression, `HydraApp.kt`):** the ChatTab header —
itself `Modifier.verticalScroll { ... }` — was nested **lexically** inside the
chat list's `Column { verticalScroll { LazyColumn { … } } }` body. The header
measures with an infinite max height ⇒ the inner `verticalScroll` throws at
measure time, on every composition. A *purely lexical* scanner misses this
class: the crash pair crosses a composable boundary (the header is a lambda
passed as a parameter, defined at file level). **Lesson: nesting detectors for
this crash class must cross composable boundaries (call-site × same-file
definition), not just count parentheses.**

**Shipped (regression is permanent, per the crash protocol AGENTS.md §7):**

| Artifact | Role |
|---|---|
| `android/.../ui/HydraApp.kt` | the fix: header de-scrolled; `AdaptiveHeaderSplit` keeps the header inside a bounded `BoxWithConstraints` height cap with `verticalScroll` applied only when the cap binds — exactly one vertical scrollable per column, invariant documented in its KDoc |
| `tools/compose_scroll_guard.py` | detector: (a) lexical same-axis nesting in strict source regions, (b) same-axis nesting across a composable boundary (call-site × definition in the same file) |
| `tests/fixtures/compose/scroll_nested_bad.kt` (4 findings), `scroll_lambda_bad.kt` (the crash shape), `scroll_ok.kt` (6 legal shapes, 0), `scroll_lambda_ok.kt` (the fix shape, 0) | negative + positive controls (R25) |
| `tools/compose_scroll_check.sh` | CI gate 1g (lint job): scans all 13 real sources, self-tests against the fixtures, aborts the build on any finding or missing file |

Verified before shipping: the **pre-fix** file (commit `d0c7679`) is detected
with exactly the real pair (`header()` call/def) and the true finding counts on
all fixtures. Guard output is parse-stable (`PASS/FAIL <path>:<line>:` lines).

**How to reproduce the gate:** `bash tools/compose_scroll_check.sh`
(exit 0 = clean; exit 1 = finding or broken fixture).

## 2. CI speed (2026-10-26) — MEASURED, CACHES SHIPPED

**Question:** user report "QEMU > 15 min per run". **Measurement**
(runs 40302888855 / 40302888863): the QEMU leg is **23 s** — the real costs
were the two **uncached** Android jobs: **APK 7 m 23 s** (fresh SDK + Gradle
every run) and **lint 4 m 38 s** (second fresh SDK). CodeQL/remaining jobs
< 1 m 15 s each.

**Shipped (cache miss = byte-identical to the old path ⇒ zero-risk):**

- `.github/actions/setup-android/action.yml` rewritten: SDK → `$HOME/android-sdk`,
  Gradle → `$HOME/gradle-dist/gradle-8.7` (cacheable `$HOME` roots instead of
  `$RUNNER_TEMP`), skip-all-downloads guard when every pinned package is
  already present (exit 0 after `sdkmanager --list_installed`), `gradle`
  input (lint.yml passes `false` — it needs the SDK but not the distribution),
  same env contract (`ANDROID_HOME`, `GRADLE_BIN`).
- `ci.yml` android-apk + `lint.yml` android-lint: `actions/cache/restore@v4`
  (SDK key **identical** in both jobs ⇒ one shared warm cache; deps key
  `gradle-deps-…-gradle8.7-<hash of android build files>` with restore-keys
  prefix) before setup; saves at job end (`SDK: if cache-hit != 'true'`;
  `deps: if success() && cache-hit != 'true'` — a red build never poisons a
  good dependency cache).
- Partial restores are safe by construction: the guard only skips when *all*
  pins are present; otherwise `sdkmanager`/Gradle add only what is missing.

**Expected:** first run after merge ≈ today (extra: one cache upload); every
subsequent Android run skips all downloads. QEMU stays as is (23 s, not worth
complicating).

## 3. Issue auto-close from CI (2026-10-26) — RESEARCHED, SHIPPED

**Question:** close issues automatically when their fix is verified.

**Research (2026 patterns):** (1) the standard mechanism is the
`gh` CLI — `gh issue close <n> --comment "…"` with
`GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}` and job-level
`permissions: issues: write` (the default token is read-only); third-party
"close-issue" actions are deprecated in favor of the CLI. (2) Reference
implementation 2026: amd/gaia PR #5408 — a *bot-alert* issue is closed only
when the check behind it actually ran and passed in the resolving run;
repeat failures edit the one open issue instead of filing new ones; a failed
lookup fails loudly, a failed resolve closes nothing. (3) GitHub-native
"Closes #N" in the PR body is the default for feature/fix issues — no
workflow at all.

**Shipped (follows the gaia semantics, no third-party action):**

- `tools/issue_verify.py` (stdlib; PyYAML optional with a flat-parser
  fallback that is itself the contract) reads `.github/issue-verifications.yml`,
  finds each mapped **open** issue via `gh api`, runs every mapped gate via
  `bash -o pipefail -c` (log → `/tmp/issue-verify.log`), and closes **only
  when every gate passed** with an audit comment (issue summary, `main@<sha>`,
  run URL, gate list, "reopen with context" note). `--dry-run` runs the gates
  but never closes. Not-found/CLOSED issues skip.
- `.github/workflows/issue-verify.yml`: `push` → main + `workflow_dispatch`,
  permissions `contents: read, issues: write`.
- `.github/issue-verifications.yml`: the mapping (first entry: issue 52).

**Convention (AGENTS.md §8):** human feature/fix issues close natively via
`Closes #N` in the PR body. Only issues that cannot be closed by a PR body
(bot-style alerts, "check for problems" requests) get a mapping entry — and a
mapping entry is a *promise*: the gates must be the actual proof, because the
close is irreversible.

## 4. Code-scanning API access from an Actions token (2026-10-26) — BLOCKED, WORKAROUNDS

`gh api /repos/JONIMONI09/BestAI/code-scanning/alerts` **and**
`/code-scanning/analyses` → **403** "Resource not accessible by integration"
(the repo Actions token lacks security-events read; a PAT from the UI would
need `Security: read`). Working channels for findings: (a) the CodeQL job logs
of the last run (annotations are printed there), (b) the Security tab in the
repo UI pasted into the issue/chat, (c) a `SECURITY_COMMAND_TOKEN`-style
fine-grained PAT if the owner adds one as a secret (not requested — do not
add secrets unilaterally).

## 5. Sandbox tool inventory + install channels (2026-10-26) — MEASURED

Arena sandbox (Debian 12, uid 1001, passwordless `sudo` present):

| Tool | State | Working install channel |
|---|---|---|
| gcc / g++ / python3 / node / npm | present | — |
| **Docker** (daemon + CLI) | absent, **not installable here** | apt blocked by egress policy (`deb.debian.org` unreachable); static binaries need `download.docker.com` (also off-allowlist); unprivileged runtime anyway. Re-test on a new sandbox: `sudo apt-get install -y docker.io && dockerd --version` |
| QEMU user-static (aarch64) | absent, not installable here | same egress block; CI's `qemu-user-static` leg remains the ARM runtime |
| aarch64 cross-compiler | absent, not installable here | **ziglang via pip works** (`python3 -m venv /tmp/zvenv && /tmp/zvenv/bin/pip install ziglang` → `zig cc -target aarch64-linux-gnu`) — proven NEON toolchain in this sandbox |
| clang / javac / java | absent, not installable here | apt block; the JNI gate's SKIP-with-disclosure path is the honest local state |
| PyYAML | present via `/tmp/yvenv` | `python3 -m venv /tmp/yvenv && /tmp/yvenv/bin/pip install pyyaml` (PEP 668 ⇒ venv) |

**Policy (AGENTS.md §6):** reachable package channels in this sandbox are
`registry.npmjs.org` and `pypi.org` (both verified PONG). **Try before
declaring a tool missing** — and record in this file what worked and what
didn't, with the exact commands.

## 6. GitHub's workflow compiler is stricter than YAML (2026-10-26) — GATE SHIPPED

The PR #53 push produced a `lint.yml` run with **zero jobs** that failed
in the same second (created_at == updated_at, no check-runs, no job log).
Cause: a duplicate `key:` line in the `actions/cache/save` mapping.
**PyYAML's `safe_load` silently accepts duplicate mapping keys** (last
one wins) — every local "YAML ok" check passed. GitHub's workflow
compiler rejects duplicates at load time, before any job exists.

Rule for this repo: any validator for a file format consumed by an
external system must model the **strictest consumer**, not the common
parser. Shipped regression: `tools/workflow_yaml_check.sh` (CI gate 1h)
+ fixture `tools/fixtures/workflow_yaml_bad.yml` (negative control, R25);
PyYAML path (runners) and stdlib indent-scan path (no PyYAML) both
verified green, including against the fixture. Full account: `.agents/errors.md`
2026-10-26 (duplicate `key:` entry).

## 7. Auto-close target changed: issues -> code-scanning alerts (2026-10-26)

Owner decision (same day, after §3 shipped and closed issue #52
successfully): CI must react to **code scanning results, not to issues**;
alerts close themselves autonomously when a fix commit makes the next
analysis clean; **no results may be hardcoded** in any workflow.

Shipped:
- **Removed:** `tools/issue_verify.py`, `.github/issue-verifications.yml`
  (hardcoded issue→gates mapping), `.github/workflows/issue-verify.yml`.
- **Added:** `.github/workflows/code-scanning.yml` — triggers on
  `code_scanning_alert` (created); every reported fact (rule id, file,
  line, message, commit, alert URL) is read at run time from the event
  payload (zero hardcoded results); delivery = PR comment when the alert's
  commit belongs to an open same-repo PR (de-duplicated by a
  `<!-- code-scanning-alert-N -->` marker), otherwise the job summary.
  Closure is **native GitHub behaviour**: the alert resolves as *fixed*
  when the next CodeQL/Semgrep analysis no longer reports it — the fix
  commit closes the alert, autonomously. Local verification: YAML +
  duplicate-key gate, shellcheck on the run script, jq parsing of a
  synthetic alert payload. Event delivery itself is verified by GitHub at
  the first real alert.
  - **Correction (same evening):** the shipped workflow could never have
    run. `code_scanning_alert` is a *webhook* event, **not** a workflow
    trigger event — GitHub's workflow compiler rejects any file that
    names it as a trigger: the run is created with **zero jobs**, fails
    in the same second, and its name is the file path (the 2026-10-25/26
    "phantom runs"; full root cause + evidence in `.agents/errors.md`,
    round 6). The workflow was **deleted the same day** and no
    alert-handling workflow was re-shipped — none can exist. The
    auto-close policy above still holds, implemented natively: the
    CodeQL/Semgrep jobs report every finding on every PR (annotations +
    check results), and GitHub resolves an alert as *fixed* when the
    next default-branch analysis no longer reports it. Regression
    prevention: **gate 1i** (`tools/workflow_lint_check.sh`, actionlint,
    in-gate negative control) now fails CI on any workflow naming an
    invalid trigger event.
- **CI tier split (same owner rule):** `ci.yml` heavy jobs (neon-test
  QEMU, build-test, gpu-bench, android-apk) run on **main pushes only**;
  PRs and feature-branch pushes get the lint-only tier (ci.yml
  `lint`/`jni-signatures`/`llama-pin` + the full `lint.yml`, which
  already runs the whole unit suite under ASan+UBSan in its
  `sanitizers` job). `release.yml` was already main/tag/dispatch only.

## 8. Local lint stack from PyPI wheels (2026-10-26) — the "no deferral" path

Owner rule: stop deferring C++ checks to CI; the sandbox has pip + gcc.
Verified 2026-10-26 (full detail in `.agents/Environment.md`, Arena section):
`python3 -m venv /tmp/lintenv && /tmp/lintenv/bin/pip install cpplint
yamllint shellcheck-py cppcheck clang-tidy` installs **pre-built wheels**
for Cppcheck 2.22.0, clang-tidy (LLVM 23.1.4), ShellCheck 0.11.0,
yamllint 1.38.0, cpplint 2.0.2 — no apt, no APT egress needed. The strict
pass (`gcc -std=c99 -Wall -Wextra -Wpedantic -fanalyzer` + `g++
-fsyntax-only -fanalyzer` + cppcheck `--enable=all` + clang-tidy
`bugprone/clang-analyzer/cert`) caught six real pre-merge defects that
day (goto crossing an initializer, missing feature-test macros, uncast
malloc, C++ `= {0}` warning, three missing final newlines, an unguarded
cd, dead variables) — all fixed, all suites green. cpplint's remaining
~180 findings are Google-C++-style noise on a C99 codebase (line length,
brace style, include order) and were deliberately not "fixed" by
reformatting the project.

## 9. Local/CI syntax drift: the five-file lesson (2026-10-26, PR #55 first run)

The first CI run of PR #55 failed in gate 1a exactly where the local
pass had not looked: the ci.yml "Syntax check (gcc, C99 strict,
-Werror)" step compiles **five** translation units
(`src/hydra_engine.c src/main.c tests/test_engine.c
tests/test_jni_batch.c tests/test_large_model.c`) **with**
`-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE`, while the local pass had
compiled two files **without** those flags. The in-file feature-test
defines (added that day for self-containment) then collided with the
command-line ones under `-Werror` (`"_DEFAULT_SOURCE" redefined`), and
the extra test TUs were never locally compiled at all.

Fix + permanent guard:
- the in-file defines are now `#ifndef`-guarded (self-contained
  standalone *and* warning-free when the same defines arrive via
  command line — both modes verified rc=0);
- new `tools/ci_syntax_mirror_test.sh` extracts the **exact** run block
  of the 1a/1b steps from ci.yml at run time (indentation-aware awk,
  nothing hardcoded) and executes it; a vacuous parse (<3 .c files or
  no command) is a hard failure. Negative control proven: against the
  pre-fix files it reproduces the CI error verbatim (redefinition →
  -Werror, rc=1); against the fix it passes.
- the same test is a step in the ci.yml lint job ("Syntax mirror
  (local/CI parity)"), so a future edit that makes the CI syntax step
  unparseable or locally broken fails CI in the same run.

Lesson (recorded per R32): a local gate is only as strong as the exact
command list and flags of the CI step it mirrors — "compiles locally"
must mean "with the files and flags CI uses", parsed from the workflow,
not copied by hand.

Second instance of the same class (same day, same PR): the mirror
itself failed on CI because ubuntu-latest's default `awk` is **mawk**,
which rejects the regex literal `/run: \|/` ("missing operand", empty
output -> the parse guard failed the step), while gawk on the local
machine tolerated it. The extraction now uses only POSIX-safe
constructs (field comparison `$1 == "run:" && $2 == "|"`,
gsub+length whitespace counting, no match()/RSTART, no POSIX classes).
Lesson: a script that mirrors CI must be tested against CI's
*interpreter*, not just the local one — and the mirror step in ci.yml
now guarantees that: it runs in both environments.

Resolution (same day, PR #55, third iteration): with the regex literal
removed, the remaining suspect was the `-v` pattern
`"Syntax check \(gcc"` — awk processes C-style escapes in `-v` values,
so mawk can deliver the pattern backslash-stripped, making the dynamic
regex an unterminated parenthesis (gawk tolerated it locally). The step
name is now matched with `index()` (plain string search); the script
now contains **no backslash escapes, no POSIX character classes, no
match()/RSTART and no -v-embedded regexes at all**. Result: the mirror
step passed on CI (`lint` job green, 23 s), the whole PR lint tier is
green (14 pass / 4 heavy jobs skipped by design), and the heavy jobs
(neon-test, build-test, gpu-bench, android-apk) now run only on the
main push after merge.

## 10. Darwin feature-test inversion: _POSIX_C_SOURCE hides, not enables (2026-10-26, release.yml red)

PR #55 merged to main; release.yml's `build-macos` job
(`clang -O3 -Wall -Wextra -Iinclude src/hydra_engine.c src/main.c -lm`,
macOS arm64, NO -D flags) failed with exactly:
`O_NOFOLLOW` undeclared, `madvise` implicit declaration,
`MADV_SEQUENTIAL` undeclared (3 errors).

Root cause: the "self-containment" fix from that morning
(in-file `#define _POSIX_C_SOURCE 200809L` / `_DEFAULT_SOURCE`) is
**glibc-centric**. glibc interprets the macros as *enablement* of
POSIX symbols; **Darwin headers interpret `_POSIX_C_SOURCE` as a
restriction** and hide BSD extensions — `O_NOFOLLOW` is not in any
POSIX version, `MADV_SEQUENTIAL` is not in any POSIX version either.
So the same in-file define enabled the needed symbols on Linux and
disabled them on macOS. Android/Bionic is tolerant (the android-apk
job passed), which is why only the macOS leg went red.

Fix + permanent guard:
- `src/hydra_engine.c` + `src/main.c`: the define block is now wrapped
  in `#if !defined(__APPLE__) && !defined(__MACH__)` (with the reason
  in a comment). Linux standalone, Linux with CI's -D flags (guarded
  #ifndef), Bionic and Darwin all verified rc=0 where measurable;
  Darwin measured on CI.
- `tools/apple_portability_check.sh` (new, R25 self-testing): asserts
  via `gcc -E -dD` that in Apple configuration the TU self-defines
  neither macro (0), in plain configuration it self-defines both
  (≥2), plus a fixture of the old broken pattern that must be
  detected (vacuous-parse guard).
- `macos-strict-scan` job added to ci.yml's **PR tier** (runs on
  pull_request, not main-only): the exact release-macos command with
  `-fsyntax-only` (no binary, no artifact — a scan, per the owner's
  "PR must find these errors and verify release-readiness without
  publishing a release") + the apple portability check. This class of
  break now fails a PR instead of a release.
- Measured negative control: forcing `-D__APPLE__` on a glibc host is
  NOT a valid Darwin simulation (glibc in strict -std=c99 hides
  O_CLOEXEC/O_NOFOLLOW/madvise even with no feature macro at all — a
  different restriction than Darwin's); the local check therefore
  covers the macro logic only and says so. The Darwin-header proof is
  the CI job.

Lesson (R32): a "portability fix" that hardcodes one libc's
feature-test semantics is a portability *bug* on every other libc;
platform-conditional feature macros need a per-platform compile proof,
and the proof belongs in the PR tier.

First run of the macos-strict-scan job (PR #56) then found the third
instance of the same drift class, inside the guard itself: step 3
(the exact release-macos compile, syntax-only) **passed** - the
Darwin fix is proven - but step 4 (apple_portability_check.sh)
failed, because assertion (2) was host-centric: it demanded that a
*plain* compile self-define both feature macros, which is correct on
a glibc host but false on an Apple host, where the plain
configuration IS the Apple configuration and must self-define
nothing. Reworked, twice: first host-aware (Apple vs non-Apple
branches, both proven by forced simulation), then **control-based** -
the assertions compare against a control TU with the same includes
and no defines, because even glibc's features.h re-emits marker
defines of `_POSIX_C_SOURCE`/`_DEFAULT_SOURCE` once its own feature
selection is active (measured: a plain Linux compile of
hydra_engine.c carries 4 such lines, 2 of them glibc's markers).
Only the difference to the control counts now; the assertions are
header-agnostic and host-consistent.
Second red of the same run (android-lint, step 9 "Kotlin-/C-
Kompilierung"): the **universal** 664-byte compile-time pin broke the
NDK **armeabi-v7a** build - 32-bit ABIs use the smaller ILP32 layout
(sizeof != 664), reproduced locally with `zig cc -target
arm-linux-gnueabihf` (the exact negative-size-array error). Fix: the
pin is now LP64-conditional (`__LP64__`/`_LP64`/`__LP64`); the
documented 664 B claim is a 64-bit statement, 32-bit targets
legitimately compile a smaller layout. Verified: x86-64 rc=0, zig
aarch64 rc=0 (pin active), zig arm-32-bit rc=0 (pin inactive), C++
parse rc=0, exact CI 1a rc=0, C suites green. Note: the PR tier
caught this **before merge** - exactly the behaviour the owner asked
for ("CI findet die Fehler auf dem PR").

## 11. CI fixture-list drift: 216 checks green locally, red on main (2026-10-31, after the round-16 merge)

**Question:** after PR #77 merged (round 16), the main-tier `build-test`
(macOS + Ubuntu) failed in the Sanitizers step (run 41222651267) while
`make test-run` stayed green locally and in the PR-tier build. What was
the drift, and how do you make the class of bug unrepeatable?

**Method:** job conclusions + step timestamps from the REST jobs API
(`gh run view --log` EOF'd repeatedly from the results-receiver — the
known log-fetch block, see .agents/Environment.md), then local
reproduction: built the 9-fixture `hydra-moe-test` from the merge commit
and ran it with the 7 argv paths the ci.yml step still inlined.

**Finding:** round 16 added two fixtures (`-q4`, `-q4f32`) to the
`hydra-moe-test` argv contract (the binary now hard-requires 9 fixture
paths, argv-count check before any test) and updated the Makefile's
`test-run` recipe — but the ci.yml sanitizer step carried its OWN inline
copy of the fixture list from before the addition. The binary refused
loudly (`fixture paths missing`); the drift was invisible on the PR tier
(lint-only by owner rule) and on local runs (Makefile updated), so the
first main-tier functional run was the first to see it.

**Resolution:** the fixture list now lives exactly once in
`tools/make_moe_fixtures.sh <base>` — it generates all nine fixtures and
prints their paths as the last stdout line; the Makefile recipe and the
ci.yml sanitizer step both call it (`… | tee <args-file>` keeps the
generator's byte-count lines in the logs, `tail -n 1` extracts the path
list). Verified: the 7-argv error reproduced before the fix; after it,
`make test-run` 234/128/7/101/152/216 all 0 and the sanitizer leg
reproduced byte-for-byte locally (ASan + UBSan, `detect_leaks=1`,
`halt_on_error=1`) → exit 0. Merged as PR #78 (`c5d281e`, main `8606d58`);
main CI 41225936406 = 9/9 success.

**Permanent artifact:** `tools/make_moe_fixtures.sh` (single source);
prevention rule — any list that must stay in sync across build entries
(fixtures, env contract, sampler ranges) is a script or a check, not a
copy (the same reasoning that produced `tools/sampler_contract_check.sh`).
Full incident record: `.agents/errors.md` (2026-10-31 entry).

## 12. Newer distro cppcheck flags round-16 test allocations (2026-11-01, main red post-#79) — FIXED, PR #80 merged `2c11d20`

**Question:** main CI run 41337186327 (the push of the round-17 docs
audit, `07bf74d`) failed exactly one step: `lint` job → "Static
analysis (cppcheck)". The identical code was green on `8606d58`
(run 41225936406). Why?

**Method:** the job log and the `lint-errors` artifact were both
unreadable from this sandbox (results-receiver EOF; artifact blob host
`productionresultssa19.blob.core.windows.net` also EOF — not on the
egress allowlist). Proof without the log, in three steps:
1. `git diff 8606d58..07bf74d -- src/ include/` = comments only
   (`hydra_moe.h`, `hydra_v2_container.c`) + `.md` files → the doc PR
   cannot change cppcheck's verdict on code.
2. Reproduce the exact CI command locally with the newest available
   cppcheck (2.22.0 from the PyPI wheel — a superset of the distro
   2.13.x era): 32 findings, all `nullPointerOutOfMemory` (27× in
   `tests/test_hydra_moe.c`) + 5× `nullPointerOutOfMemory` + 1×
   `nullPointerArithmeticOutOfMemory` (512-cases block already checked
   at line 2107, the two pinned-extremes/asymmetry blocks were not).
3. All findings sit in the round-16 test code (KV cache helpers added
   in PR #77) — pre-existing, invisible to the older distro cppcheck,
   visible to the newer one. The runner image was refreshed between
   the two runs; the cppcheck version change is the drift vector
   (same event that also dropped `gcc-12-aarch64-linux-gnu`, §13).

**Resolution:** `kv_set_alloc`/`kv_set_free` helpers in
`tests/test_hydra_moe.c` own the whole per-layer cache chain (checked
calloc + per-layer malloc, full cleanup on OOM, no leak, no NULL
reaches `hydra_kv_cache_init`/`hydra_moe_forward`); all five KV test
sites use them; the eviction sink's `k0/v0/ak/av` and the four
unchecked `ref_get` calls got checks; the two `tests/test_engine.c`
blocks got the file's own OOM convention. Post-fix: cppcheck 2.22
(exact CI command) 0 findings; tests 234/128/7/101/152/216 all 0
(baseline unchanged — the OOM paths never execute in practice);
ASan+UBSan moe+engine clean.

**Lesson:** unversioned apt packages in CI are a drift vector in BOTH
directions — a newer version can newly REJECT code (this incident) and
an image refresh can DELETE a pinned old version (§13). A code fix
that is clean under the newest superset tool is the durable fix;
pinning the old version would just move the failure to the next image
refresh.

## 13. Runner image drop: gcc-12-aarch64-linux-gnu no longer in apt sources (2026-11-01) — FIXED in ci.yml + release.yml

**Question:** two main-tier jobs failed at their first real step after
the #80 merge: `CI` → `neon-test` (run 41339831236) and `Release` →
`CLI (Linux)` (run 41339831264), both at "Install AArch64
cross-compiler and qemu-user" / "Install cross toolchain + qemu" —
`sudo apt-get install -y -qq gcc-12-aarch64-linux-gnu qemu-user-static`.
The same step succeeded on the previous image (e.g. the 07bf74d
Release run's sibling jobs all passed; only the apt step failed).

**Method:** job steps via the Jobs API (`/actions/jobs/{id}` — the
steps endpoint that works while the log endpoint EOFs): both failures
are the apt install step, everything downstream skipped. The identical
step failed on two consecutive runs two days apart on two different
commits → not transient, the package is gone from the current image's
apt sources. Consistent with the same image refresh that bumped
cppcheck (§12) — one event, two symptoms.

**Resolution:** the unversioned cross-compiler
(`gcc-aarch64-linux-gnu` → `aarch64-linux-gnu-gcc`, whatever gcc the
image ships) in both workflows, plus `aarch64-linux-gnu-gcc --version`
printed in the install step so a future toolchain drift shows up in
the log. The gates that make the version non-load-bearing already
exist: the test-count drift gate (ARM baseline), the NEON-vs-scalar
comparison tests, and the x86-vs-ARM/NEON bit-identical parity check
in both workflows. If any future compiler change broke NEON
behaviour, those gates would catch it — the pin was buying nothing
that the gates don't already prove.

**Lesson:** in CI, prefer "default + prove it" (unversioned tool +
behaviour gates) over "pin the exact old version" — pins rot when the
image moves, behaviour gates don't.
