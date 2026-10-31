---
name: ci-triage
description: "Triage a red GitHub Actions run of the Hydra-Stone repo: read check-run annotations when logs and artifacts are blob-blocked, recognize the zero-job compile-failure signature, and reproduce the failing gate locally. Use when CI is red, a run has no jobs, or a job failed without a readable log."
---

# CI Triage — Hydra-Stone

How to find the REAL cause of a red CI run in this repo, without
guessing. All techniques below were verified on live runs
(2026-10-25/26); see `.agents/errors.md` for the incident log.

## When to use

- Any job of `CI`, `Lint & Security`, `Release` fails on a PR or on main.
- A run was created with **0 jobs** and failed in the same second.
- You need the error text but the job log / artifact download is
  blocked (this sandbox cannot fetch them: `gh api .../logs` and
  artifact zip downloads die with a token/EOF error).

## Step 1 — Map the run before reading anything

```bash
gh api "repos/JONIMONI09/BestAI/actions/runs?branch=<branch>&per_page=5" \
  --jq '.workflow_runs[] | {id, name, event, status, conclusion, created_at}'
gh api repos/JONIMONI09/BestAI/actions/runs/<run_id>/jobs --jq \
  '.jobs[] | select(.conclusion != "success" and .conclusion != "skipped") | {name, conclusion}'
```

Know which tier you are on: **PR tier runs CHECKS only**
(build-test/neon/apk/gpu skip by design on PRs); the main-only tier
jobs only run on pushes to main. A skipped job is not a failure.

## Step 2 — Read the check-run ANNOTATIONS (the log alternative)

Job logs and artifact downloads are blob-blocked from this sandbox,
but annotations are readable:

```bash
gh api repos/JONIMONI09/BestAI/check-runs/<job_id>/annotations --jq \
  '.[] | .annotation_level + " | " + (.path // "n/a") + ":" + (.start_line|tostring) + " | " + (.message[:300])'
```

Compiler diagnostics (kotlinc "Unresolved reference …", clang
`-Werror` lines) are published as annotations; the repo's own gates
also publish `::error::`/`::warning::` annotations deliberately for
this reason (the Android lint gate prints the gradle tail into an
annotation). Read the annotations FIRST, always.

If a gate only prints PLAIN output + an exit code (e.g. cppcheck with
`--error-exitcode=2` shows nothing but "Process completed with exit
code 2"), the annotations cannot tell you WHICH check fired. Then add
a TEMPORARY diagnostic step right after the gate: same command,
`--error-exitcode=0` (or a no-op exit), `if: always()`, every output
line re-emitted as `echo "::error::[gate] $line"`. Push, read the new
annotations, fix the findings, then REMOVE the diagnostic step in the
final commit. Verified live 2026-10-30 (R12 cppcheck incident,
errors.md). Two caveats learned there: a failure annotation's
`start_line` points INSIDE the failing step's run block (locate the
step, don't trust the line), and cppcheck ABORTS at the first
error-severity finding — re-run the analysis to 100% after fixes.

## Step 3 — Recognize the zero-job signature

A run with **0 jobs**, `conclusion: failure`, `created_at ==
updated_at`, run NAME = the file PATH (not the workflow `name:`), no
check-runs, no timing = **the workflow file failed to COMPILE**
(GitHub rejected it at load time). Do not chase "server-side
corruption": diff the workflow file against the last green version and
validate locally:

```bash
bash tools/workflow_yaml_check.sh   # duplicate keys (PyYAML is too permissive alone)
bash tools/workflow_lint_check.sh   # actionlint: invalid events/actions, unknown steps
```

Incident: `on: code_scanning_alert` is a webhook event, not a workflow
trigger event — every file naming it produced this signature
(2026-10-26). The phantom run ALWAYS names the offending file.

## Step 4 — Reproduce the failing gate LOCALLY (no deferring, owner rule)

| CI gate | Local replica (all verified, see Environment.md) |
|---|---|
| gate 1a gcc strict | exact command from `.agents/skills/engine-ci-verify/SKILL.md` |
| gate 1b clang strict | `/tmp/zvenv/bin/python-zig cc -target x86_64-linux-gnu -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Iinclude -Isrc -Iandroid/app/src/main/cpp -c <file> -o /dev/null` (zig bundles a real Clang frontend; apt has no clang) |
| gate 1c cppcheck | NOT reachable in the current sandbox (2026-10-30: the proxy whitelist serves llama.cpp/cpython/git/openssl/... but 404s cppcheck/cppcheck for codeload+git+api; no root for apt; pypi only has a useless 1.5.3 wrapper). Fallback per the annotation recipe in Step 2: a temporary diagnostic step that re-emits the CI cppcheck output as annotations. Local substitute for C-code review: `gcc -fanalyzer -std=c99 -Wall -Wextra -Werror` + ASan/UBSan. |
| Kotlin compile (Android lint/APK) | `bash tools/kotlin_stub_check/kotlin_stub_check.sh` (kotlinc via npm; stub check 0 errors required) |
| workflow compile | the two workflow gates above |
| macOS portability | `python-zig cc -target arm64-apple-darwin -fsyntax-only ...` (compile only) |

If a tool's /tmp install is gone: recreate it from the exact commands
in `.agents/Environment.md` (Arena section) — do not improvise.

## Step 5 — Report the fact, not the theory

- State the exact annotation/compiler line that is red.
- State which local replica reproduced it (command + output).
- Never ship a fix "that should work" — the local replica must be
  green for the same flags (R7, R25, R32).
