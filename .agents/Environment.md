# Environment.md — execution environments of the AI agents working on this repo

> **What this file is.** A shared, in-repo record of the *execution
> environments* (sandboxes, toolchains, network policies) in which AI
> agents work on this repository — and, crucially, **which checks can and
> cannot be run locally in each environment**, with evidence: exact
> commands, exact errors, dates.
>
> It exists for two readers:
> 1. **The agent that owns a section** — to record what works, what is
>    blocked, and how each block was worked around.
> 2. **Every other agent (any provider, any session)** — to reconstruct
>    the environment without re-discovering it: if another agent already
>    recorded a working path for a tool you lack, **try that path first**.

## Contract (binding for all agents — see AGENTS.md §6)

1. **Only update your own section.** Sections are owned by
   *provider + sandbox type* (e.g. `### Arena AI — Debian 12 container`).
   You may append dated entries to your own section and add a new section
   for a new environment. **Never edit, rewrite or delete another
   agent's section** — supersede it by adding your own entry, dated.
2. **No silent deferral to CI.** A check that *can* be run locally in
   your environment **must be run locally** before it is left to CI.
   "CI will check it" is not a record; the local run and its output are.
3. **Record every block with evidence.** When something does not work:
   the exact command tried, the exact error message (trimmed), the date,
   the *reason* (measured, not assumed), and — if you found one — the
   workaround. A block without a date and a reason is not a block, it is
   a guess.
4. **Keep sections current.** If a previously blocked tool becomes
   available (new sandbox, new channel), run the deferred checks, record
   the results under a new dated entry, and mark the old entry superseded.
5. **This file is a starting point for every new session.** Read the
   section that matches your environment before the first tool
   invocation; follow its tested paths instead of re-deriving them.

**Template for a new section:**

```markdown
### <Provider> — <sandbox type / OS>
- Recorded: <YYYY-MM-DD> by <agent id/name>
- **What it is:** container? OS + version? root/sudo? workspace root?
  persistent /tmp?
- **Network (egress):** which hosts are reachable (allowlist) and which
  are not — measured, with the date.
- **Present tools:** ... (with versions where it matters)
- **Blocked tools:** for each — date, exact command, exact error, reason,
  workaround (or "none found").
- **Tested local check paths:** the exact commands that run the repo's
  gates locally (compiler, linters, test suites) and their latest result.
```

---

### Arena AI — Debian 12 container (E2B-style)

- Recorded: 2026-10-26 by the Arena AI agent (session branch
  `arena/91276612-bestai`); earlier entries 2026-10-25/26 superseded
  where contradicted below.
- **What it is:** a container on the Arena.ai Agent Mode platform
  (E2B-style): Debian GNU/Linux 12 (bookworm), non-root user (uid 1001)
  with passwordless `sudo` present, workspace root `/home/user` (the repo
  is cloned at `/home/user/BestAI`). `/tmp` is **ephemeral** — recreated
  venvs from earlier sessions are gone; the recreate command is recorded
  below so this costs seconds, not discovery time.
- **Network (egress, measured 2026-10-26):** allowlist — `github.com`,
  `codeload.github.org`, `api.github.org`, `api.github.com`,
  `registry.npmjs.org`, `pypi.org`, `files.pythonhosted.org` — everything
  else unreachable. Consequences (all measured, not assumed):
  - `deb.debian.org` (APT) **unreachable** → `apt-get` cannot install
    anything (not even a dry-run fetch succeeds).
  - `download.docker.com` / Docker Hub unreachable → **Docker cannot be
    installed** (neither package nor static binary). No container runtime
    is required by any gate of this repo; do not let a future check start
    needing one.
  - `dl.google.com`, `services.gradle.org` unreachable → no Android SDK /
    NDK / Gradle locally → Kotlin/NDK/APK work is CI-only (jobs
    `android-apk`, `android-lint`, cached since 2026-10-26).
  - No QEMU → ARM *execution* impossible; ARM *compilation* is possible
    (zig path below).
- **Present tools (2026-10-26):** `gcc`/`g++` 12.2 (with `-fanalyzer`),
  `make`, `python3` 3.11.2 (PEP 668 → use a venv), `node` + `npm`
  (npm registry reachable), `git` + `gh` (GitHub auth configured),
  `jq`, `curl`, `unzip`.
- **Blocked tools (with evidence):**
  - **Docker / podman / nerdctl** — 2026-10-26: `sudo apt-get install
    -y docker.io` → `E: Unable to locate package docker.io` (after
    `apt-get update` failed with `Err: http://deb.debian.org/...`).
    Reason: egress allowlist. Workaround: none — not needed (no gate
    requires a container runtime).
  - **QEMU (user or system)** — 2026-10-26: same APT block. Workaround:
    CI's `neon-test` (now main-only) is the ARM runtime; for pure
    *compile* checks of the ARM NEON path: the zig path below.
  - **System clang / cppcheck / aarch64-gcc** — 2026-10-26: APT block.
    Workaround: **pip pre-built wheels work** (next section) — cppcheck
    and clang-tidy install and run fine from PyPI.
  - **Code-scanning alerts API** — 2026-10-26: `gh api
    /code-scanning/alerts` and `/code-scanning/analyses` → 403
    "Resource not accessible by integration" (the repo Actions token has
    no security-events scope). Workaround: CodeQL job logs /
    check-run annotations API (works), or the Security tab UI (human).
    Alert *handling* itself needs no API: GitHub delivers
    `code_scanning_alert` events to
    `.github/workflows/code-scanning-alert.yml` (added 2026-10-26, renamed
    from `code-scanning.yml` 2026-10-26 — see the corrupted-registration
    note in the round-6 block below) and resolves alerts natively when the
    next analysis no longer reports them.
- **Tested local check paths (the ones to run — owner rule: no deferring
  C++/lint checks to CI, 2026-10-26):**
  1. **Linter venv (recreate if `/tmp/lintenv` is gone):**
     ```bash
     python3 -m venv /tmp/lintenv
     /tmp/lintenv/bin/pip install --disable-pip-version-check \
         cpplint yamllint shellcheck-py cppcheck clang-tidy flawfinder
     ```
     Verified 2026-10-26: installs **Cppcheck 2.22.0** (wheel),
     **clang-tidy (LLVM 23.1.4)** (wheel), **ShellCheck 0.11.0**,
     **yamllint 1.38.0**, **cpplint 2.0.2**, **flawfinder 2.0.20**
     (PyPI re-implementation, `flawfinder --minlevel=4 src/ include/`
     → "No hits found" at that level, 2026-10-26) — all from PyPI, no apt.
  2. **C/C++ static analysis (run on every change to `src/`, `include/`,
     `tests/`):**
     ```bash
     /tmp/lintenv/bin/cppcheck --enable=all --std=c99 --platform=unix64 \
         -Iinclude src/hydra_engine.c src/main.c
     /tmp/lintenv/bin/clang-tidy --checks='-*,bugprone-*,clang-analyzer-*,cert-*' \
         --extra-arg=-Iinclude --header-filter='include/|src/' \
         src/hydra_engine.c src/main.c
     gcc -std=c99 -fsyntax-only -Wall -Wextra -Wpedantic -fanalyzer \
         -Iinclude src/hydra_engine.c src/main.c
     g++ -fsyntax-only -Wall -Wextra -Wpedantic -fanalyzer \
         -Iinclude src/hydra_engine.c src/main.c   # C++ parse: the files
                                                   # must survive it
     ```
     Baseline 2026-10-26 after this round's fixes: **gcc/g++ 100 %
     clean**; cppcheck `--enable=all` → 0 error/warning (remaining:
     `style` unusedFunction/staticFunction on the exported M4
     primitives — by design, they are exercised by the test binaries;
     the CI gate is `--enable=warning,portability`); clang-tidy → no
     real-analyzer findings (remaining: intentional signed bit ops of
     the pinned LCG/2-bit packing, `fprintf(stderr)` return by
     convention, C11-`_s`-API style noise, 3 easily-swappable-params on
     M4 API signatures).
  3. **Shell + YAML:**
     ```bash
     /tmp/lintenv/bin/shellcheck -S warning tools/*.sh
     /tmp/lintenv/bin/yamllint -d "{extends: default, rules: {line-length: {max: 200}, document-start: disable, truthy: disable, comments: {min-spaces-from-content: 1}, indentation: {spaces: consistent}}}" .github/
     bash tools/workflow_yaml_check.sh   # duplicate keys: GitHub rejects what PyYAML accepts
     ```
  4. **The standard 5-part gate** (AGENTS.md §3) runs fully in this
     sandbox: `make clean && make && make test && make test-run`,
     `./hydra-test`, `./jni-batch-test`, `./large-model-test`,
     `tools/test_count_check.sh`, `tools/gguf_test.py --engine
     ./hydra-run`, `node --test tools/server_test.js`,
     `tools/console_train_test.js`, `tools/contrast_check.js`, and all
     `tools/*_check.sh` guardrails (the JNI gate SKIPs without javac and
     says so — R23-honest).
  5. **ARM NEON compile-only substitute** (no qemu for execution):
     ```bash
     python3 -m venv /tmp/zvenv && /tmp/zvenv/bin/pip install ziglang
     mkdir -p ~/.cache/zig
     # NOTE: zig needs ABSOLUTE -I paths (a relative -Iinclude is not
     # resolved against the cwd and dies with "hydra_model.h: No such
     # file or directory", 2026-10-26)
     /tmp/zvenv/bin/python-zig cc -target aarch64-linux-gnu \
         -I/home/user/BestAI/include -Wall -Wextra \
         /home/user/BestAI/src/hydra_engine.c /home/user/BestAI/src/main.c \
         -o /tmp/hydra_arm   # full compile, rc=0 (incl. the 664-byte size pin)
     ```
     (2026-10-26, ziglang 0.17.0 wheel. Two 0.17 quirks measured: the
     console script is **`python-zig`, not `zig`**, and
     `-fsyntax-only` is broken — zig maps it to `-c` and then fails
     renaming into `~/.cache/zig//o/` (FileNotFound, directory absent).
     A **full compile** works and is the stronger check anyway.)
     Compile check only — *running* `/tmp/hydra-test-arm` needs QEMU,
     which CI's `neon-test` provides (main-only since 2026-10-26).
- **Findings this path produced (2026-10-26, proof that local runs beat
  deferral):** the strict C99 + C++-parse pass above caught six real
  defects before CI ever saw them — a `goto` crossing an initializer
  (ill-formed in C++, `hydra_engine.c`), missing feature-test macros
  under strict `-std=c99` in both `src/` files, an uncast `malloc` and a
  `= {0}` aggregate (C++ parse), missing final newlines in
  `src/hydra_engine.c` / `src/main.c` / `.github/workflows/lint.yml`, an
  unguarded `cd` in `tools/workflow_yaml_check.sh`, and dead variables in
  `tools/string_format_negative_test.sh`. All fixed; all suites green.

- **Round 6 additions (2026-10-26, branch `arena/ed8432d1-bestai`):**
  1. **Kotlin compiler WITHOUT the Android SDK — npm path (tested):**
     Maven Central and GitHub release assets are not on the egress
     allowlist (`objects.githubusercontent.com` → curl exit 35), and the
     PyPI `kotlin`/`kotlin-compiler` packages are junk/absent. The npm
     registry, however, ships a full kotlinc:
     ```bash
     mkdir -p /tmp/ktnpm && cd /tmp/ktnpm
     npm install kotlin-compiler@2.4.21 javajre-linux-64@17.0.8
     export PATH=/tmp/ktnpm/node_modules/javajre-linux-64/jre/bin:$PATH
     /tmp/ktnpm/node_modules/kotlin-compiler/bin/kotlinc -version
     # -> kotlinc-jvm 2.4.21 (JRE 17.0.8+9-LTS-211), matching CI's
     #    Temurin 17 major
     ```
     Combined with the stub classpath in
     `tools/kotlin_stub_check/stubs/` (40 hand-written .kt files mirroring
     the exact android/androidx/compose/org.json API surface the app uses,
     R generated from the real `res/values/strings.xml`), this type-checks
     ALL app Kotlin locally in ~20s:
     `bash tools/kotlin_stub_check/kotlin_stub_check.sh`.
     **This is the local equivalent of the CI Kotlin-compile step** and is
     what caught the round-6 `ClosedFloatingPointRange.last/first` error
     and the missing `androidx.compose.foundation.layout.weight` import
     before CI. Limits (honest, R32): it verifies Kotlin type resolution
     and signatures only — resources, manifest, the compose-compiler
     plugin and lint checks still need the real AGP build in CI. Stub
     fidelity rules that cost real iterations: verify every stubbed
     signature on the web first (Compose `LazyListScope.items` key lambda
     is single-parameter `(item: T) -> Any`; two parameters is
     `itemsIndexed`); one package per file; `fun interface` for Java SAM
     conversions (plain `interface` + lambda does not convert); a stub
     `var` and a stub `fun` with the same JVM signature clash (only one of
     `isEnabled`/`setEnabled`); generic base classes inherited with a
     no-arg super call need an explicit `()` on the base constructor.
  2. **Cppcheck package name (tested):** the working PyPI package is
     **`cppcheck-py`** (`pip install cppcheck-py` → Cppcheck 2.22.0, 2026-
     10-26). Plain `cppcheck` on PyPI does not provide the binary — the
     venv recreate line in this section's linter block used the long
     package list; `cppcheck-py` is the one that resolves the `cppcheck`
     entry point. (Both names recorded so the next session does not
     rediscover this.)
  3. **GitHub Actions logs + artifacts are blob-blocked from this
     sandbox (2026-10-26, measured):** `gh run view --log-failed` → "log
     not found" / EOF on the blob storage for FAILED runs (CI, Lint,
     Release, code-scanning on main f79ff96 all hit this). **Working
     diagnostic path:** the check-run annotations API reads the findings
     the job itself emitted:
     ```bash
     gh api repos/JONIMONI09/BestAI/actions/runs/<runId>/jobs \
         --jq '.jobs[] | select(.conclusion=="failure") | {id, name}'
     gh api repos/JONIMONI09/BestAI/check-runs/<jobId>/annotations \
         --jq '.[] | {path, line: .start_line, annotation_level, message}'
     ```
     This is how the `HydraApp.kt:1011` Unresolved reference 'last'
     finding was recovered despite zero readable logs. (APIs that DO
     work from here: runs, jobs, check-runs + annotations, workflows,
     pulls, issues. APIs that are 403: actions/permissions,
     code-scanning/alerts.)
  4. **PyYAML for the workflow gate (tested):** `pip install pyyaml`
     (PEP 668 → `--break-system-packages` or a venv) upgrades
     `tools/workflow_yaml_check.sh` from the stdlib duplicate-key scan to
     a full parse. The stdlib fallback genuinely cannot see syntax errors
     — it is not a parser — so for any change to `.github/workflows/` the
     venv-with-PyYAML variant is the stronger local check. All 5 workflows
     parsed clean 2026-10-26 with PyYAML 6.x.
  5. **Corrupted workflow registration (the 3rd CI-red root cause, 2026-
     10-26):** the `code_scanning_alert` workflow was registered the day
     it was created with the file PATH as its name and a broken trigger
     set — it executed on EVERY push (3 consecutive runs, event=push, 0
     jobs, failure in <3s, no job logs: the "zero-job" signature described
     by `tools/workflow_yaml_check.sh`). The file itself is valid (PyYAML
     + duplicate-key gate clean), so no amount of local YAML checking
     could have caught it; the fix is a re-registration, done by moving
     the file to `.github/workflows/code-scanning-alert.yml` (same
     content). Detection recipe: `gh api .../actions/workflows` (registered
     name should equal the file's `name:` key), then compare against the
     run's event field. After the merge to main, the orphaned
     `code-scanning.yml` registration is expected to clear with the file
     deletion; watch the next main push for any new zero-job runs.
  6. **`/tmp` wipe confirmed again (2026-10-26):** `/tmp/ktnpm`,
     `/tmp/lintvenv` and every earlier session's venv were gone at session
     start; both recreate commands are recorded above (npm for the Kotlin
     toolchain, pip for the lint venv). Anything that must survive
     between turns lives in the workspace — the stub classpath was
     committed to `tools/kotlin_stub_check/` for exactly this reason.
