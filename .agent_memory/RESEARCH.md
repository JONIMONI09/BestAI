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
| 2026-10-26 | Auto-close target: issues → code-scanning alerts | issue-verify removed; event-driven workflow shipped |
| 2026-10-26 | Local lint stack from PyPI wheels (no apt needed) | 6 real pre-merge defects caught; path in Environment.md |

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
verified green, including against the fixture. Full account: `errors.md`
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
- **CI tier split (same owner rule):** `ci.yml` heavy jobs (neon-test
  QEMU, build-test, gpu-bench, android-apk) run on **main pushes only**;
  PRs and feature-branch pushes get the lint-only tier (ci.yml
  `lint`/`jni-signatures`/`llama-pin` + the full `lint.yml`, which
  already runs the whole unit suite under ASan+UBSan in its
  `sanitizers` job). `release.yml` was already main/tag/dispatch only.

## 8. Local lint stack from PyPI wheels (2026-10-26) — the "no deferral" path

Owner rule: stop deferring C++ checks to CI; the sandbox has pip + gcc.
Verified 2026-10-26 (full detail in `Environment.md`, Arena section):
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
