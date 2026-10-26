# AGENTS.md — working guide for AI coding agents in this repository

> Read this before touching anything. It is the single entry point for an
> agent (or a human new to the tree) and is written so that following it is
> a complete session: protocol first, architecture second, verification last.
> All documentation and code comments in this repository are English
> (rules.md R31).

## 1. Mandatory skills and rules (read first, enforce always)

| File | What it binds you to |
|---|---|
| `rules.md` | 32 standing rules (R1–R32). Highest priority is the session protocol (R1–R4). When rules conflict: safety > correctness > session protocol > documentation > convenience (rules.md §8). |
| `.claude/skills/session-workflow/SKILL.md` | Read `session.md`, `errors.md`, `rules.md` BEFORE any code. Update `session.md` after every completed step. Log every error in `errors.md` the moment it is hit. A session is not complete without both files updated. |
| `.claude/skills/engine-ci-verify/SKILL.md` | The local quality gates, in CI-identical order, before any commit touching `src/`, `include/`, `tests/` or `tools/`. |
| `.claude/skills/android-ndk-build/SKILL.md` | The Android toolchain bootstrap, APK build, no-KVM emulator verification and the 8 known pitfalls. Full long form: `docs/ANDROID_SKILL.md`. |

The non-negotiable substance, in one breath: no fake tests (R5), no claimed
capabilities (R6), prove claims by execution (R7), PoC before and after every
security fix (R8), compare parallel implementations against each other (R9),
validate every length-deriving field against real bounds (R14), pin
toolchains (R15), measure before claiming (R32), one logical change per PR
and nothing merged without the user (R17/R18), and never `git checkout`
(R20).

## 2. Architecture map

One C engine, four consumers. The engine has exactly one implementation
(`src/hydra_engine.c`, R12); everything else wraps it or converts into it.

```
                    ┌────────────────────────────────────────────────┐
                    │           include/hydra_model.h                │
                    │  format constants, header, HydraEngine context │
                    └────────────────────────────────────────────────┘
                                      ▲
┌────────────────────┐   mmap/step    │   ┌─────────────────────────┐
│ src/hydra_engine.c │◄───────────────┼───│ src/main.c  (CLI)       │
│ + src/hydra_neon.h │  hydra_engine_ │   │ hydra-run <model> seed  │
│  (AArch64 kernel)  │  load/step/…   │   │   <steps> [--json]      │
└────────────────────┘                │   │       [--prompt][--bench]
        ▲      ▲                      │   └─────────────────────────┘
        │      │ JNI (libhydra.so)    │        ▲
┌───────┴──┐ ┌─┴──────────────────────┴─┐      │ spawns as child
│ hydra_jni│ │ server.js (Node console) │◄─────┘
│ .c       │ │ /api/* + static public/  │   (execFileSync of hydra-run;
│ +hydra_  │ │ zero runtime deps        │    the JS never re-implements
│ batch.h  │ └──────────────────────────┘    the math)
└──────────┘
        ▲
┌───────┴────────────────────────────────────┐
│ android/app/src/main/java/dev/hydrastone/  │
│  EngineInterface.kt  ← the ONLY interface  │
│   ├─ HydraEngine.kt   (v1 .hydra ternary)  │
│   └─ LlamaEngine.kt   (llama.cpp GGUF)     │
│  HydraBridge.kt / LlamaBridge.kt (JNI decls│
│  checked against native by                 │
│  tools/jni_signature_check.sh)             │
│  AtomicModelSwap.kt (backup/rename/restore)│
│  MainActivity.kt → ui/HydraApp.kt (Compose)│
└────────────────────────────────────────────┘
        ▲
┌───────┴────────────────────────────────────┐
│ android/app/src/main/cpp/llama_jni.cpp     │
│  + llama/ (submodule, llama.cpp v0.5.0     │
│    pinned at 7fe450e…, MIT — do not bump   │
│    without tools/llama_pin_check.sh)       │
└────────────────────────────────────────────┘
```

### 2.1 The C engine (`src/hydra_engine.c`)

A ternary recurrent model: per token,
`acc[i] = A[i]·token + B[i]·state[i]`, `state[i] = clamp(acc[i], ±127)`,
and the output token is a symmetric-modulo function of `acc[0]`. The weight
file is a 24-byte little-endian header plus packed ternary weights (v1: two
weights per byte; an accepted undocumented-then-documented version=2 variant
packs four per byte — see `docs/FORMAT.md`). The loader mmaps the file
(read-only, `O_NOFOLLOW`), validates five bounds, and **aggregates all layers
once at load** into `A[i]/B[i]`; the per-token path never touches the weight
region again. NEON on AArch64 vectorises the aggregated loop; the scalar
tail and the whole x86 path share one code shape. `hydra_engine_step_fast()`
is a token-only path whose stale-state contract is documented in
`include/hydra_model.h` and tested in `tests/test_engine.c`.

### 2.2 The GGUF/llama.cpp engine (Android)

`llama_jni.cpp` wraps llama.cpp (pinned submodule) with one global session,
a mutex, and quit/cancel atomics (set before the lock so an in-flight
generation unwinds; cleared by an RAII guard so a stale stop cannot kill the
next run). The sampler chain is rebuilt only when the temperature changes
(the chain bakes the temperature in), with a fixed seed (R22) and NaN-safe
clamping. `LlamaEngine.kt` reports llama.cpp's own `state_bytes` and
`size_bytes` — the RAM figures on screen are the library's allocations, not
estimates. `AtomicModelSwap.kt` makes an import a backup→rename→(restore on
failure) operation: a failed swap is a no-op.

### 2.3 The Python conversion pipeline (`tools/`)

`gguf_reader.py` (stdlib only) reads GGUF v2/v3: F32/F16/BF16 dense plus Q8_0
block dequant (34-byte blocks, `w = d·q`); everything else is refused **by
name**. `gguf_to_hydra.py` converts a GGUF into a v1 `.hydra`: it ternarises
the real float weights with the b1.58 absmean rule (threshold `mean|W|/2`,
rounding half-away-from-zero), sums per column, folds to 64 dimensions,
rescales to peak ±127 and emits a file whose aggregation equals the computed
`A/B` **exactly** (each `A[i]` is written as that many ±1 layers). The
result is an honest projection into the recurrent model the format can
express — not a Llama; the report says so. `gguf_test.py` is the gate: an
independent re-implementation of the packing, an independent re-implementation
of `step()`, a real `hydra-run` comparison, and 12 negative controls.
`make_model.py` is the canonical v1 starter writer (it asserts the starter is
not state-blind). `gguf_inspect.py` answers "is this file convertible" before
a long conversion. `gguf_speed.py` measures the converter (wall time,
throughput, peak RSS) — re-run it rather than quoting a stale number.

### 2.4 The Node training/web suite

`server.js` is a zero-dependency console: static UI (`public/`) plus
`/api/{models,model,models/upload,models/import,models/inspect,infer,train,
vocab,axiom,cancel,errors}`. All model paths go through realpath (R28);
uploads are size-capped with a streaming guard that answers 413 **before**
destroying the socket (errors.md: "The 413 that was never delivered");
`/api/models/import` is single-active with HTTP 429 + `Retry-After` on the
second caller. The heavy lifting — inference and training — runs in the
compiled C engine and in `tools/hydra_train.js`, whose `step()` is a
line-by-line mirror of `hydra_engine_step()` and which hill-climbs on the
whole-sample score (ternary moves, best snapshot returned). The console
therefore never has its own math to drift from the engine.

### 2.5 What each surface must never do

- No second copy of the engine, the batching policy, or a decision rule
  (R12): if a rule lives in two places, one is a future bug.
- stdout is data, stderr is logs (R13): `--json` output must stay parseable.
- No capability may be documented that is not wired up (R6) — and, equally,
  a wired-up path must not go undocumented (this session's finding F-1,
  `session.md` 2026-10-25).
- No internal detail in an error response (R27): the user gets an
  actionable sentence; paths/stacks go to the log.

## 3. Verification gates (the exact commands)

Run in this order; a red gate stops the session. (Full rationale and the
optional ARM/NEON gates: `.claude/skills/engine-ci-verify/SKILL.md`.)

```bash
# 0. Environment (once per machine)
#    If a gate's tool is missing in your sandbox: read Environment.md
#    FIRST - another agent may have recorded a tested install path for
#    exactly your environment (owner rule 2026-10-26: no silent deferral
#    of C++/lint checks to CI; try the recorded path before giving up).
#    Afterward, record your own findings in YOUR section only.
make clean && make && make test            # builds engine + ALL test binaries
make test-run                              # RUNS them (test-run, not test)

# 1. Native engine suites
./hydra-test                               # expect "N Tests, 0 failures" (N parsed, never assumed)
./jni-batch-test                           # expect "86 checks, 0 failed" (count parses from output)
./large-model-test                         # 5 GiB sparse model + 32-bit offset refusals
bash tools/test_count_check.sh ./hydra-test x86   # parsed count vs reviewed baseline (R21)

# 1b. C/C++ static analysis (mandatory locally on any src//include/ change —
#     owner rule 2026-10-26: do NOT defer this to CI). Tool install paths
#     per environment: Environment.md (in the Arena sandbox the tested
#     path is the /tmp/lintenv pip venv: cppcheck 2.22, clang-tidy, ...).
/tmp/lintenv/bin/cppcheck --enable=all --std=c99 --platform=unix64 \
    -Iinclude src/hydra_engine.c src/main.c
/tmp/lintenv/bin/clang-tidy --checks='-*,bugprone-*,clang-analyzer-*,cert-*' \
    --extra-arg=-Iinclude --header-filter='include/|src/' \
    src/hydra_engine.c src/main.c
gcc -std=c99 -fsyntax-only -Wall -Wextra -Wpedantic -fanalyzer \
    -Iinclude src/hydra_engine.c src/main.c
g++ -fsyntax-only -Wall -Wextra -Wpedantic -fanalyzer \
    -Iinclude src/hydra_engine.c src/main.c     # C++ parse must stay error-free
bash tools/ci_syntax_mirror_test.sh             # the EXACT ci.yml syntax commands
                                               # (5 TUs incl. tests, -D flags,
                                               # -Werror) — local may never drift
                                               # from CI (RESEARCH.md section 9)

# 2. Python pipeline
python3 tools/gguf_test.py --engine ./hydra-run   # expect "N checks, 0 failed" (N parsed, never assumed)
python3 tools/gguf_speed.py                        # converter throughput (host-labelled)
python3 tools/string_format_check.py               # format-string/specifier contract
bash tools/string_format_negative_test.sh          # the gate must be able to fail (R25)

# 3. Node server, trainer, UI
node --test tools/server_test.js           # 37 live HTTP tests
node tools/console_train_test.js           # trainer vs the COMPILED engine, bit-for-bit
node tools/contrast_check.js               # WCAG AA, 17 pairs

# 4. Repository guardrails
bash tools/jni_signature_check.sh          # Kotlin<->C JNI contract (SKIPs without javac — say so)
bash tools/android_header_check.sh
bash tools/android_autostart_check.sh
bash tools/android_autostart_negative_test.sh
bash tools/android_crash_check.sh
bash tools/llama_pin_check.sh              # llama.cpp pin + MIT licence
bash tools/llama_pin_negative_test.sh
bash tools/docs_claim_check.sh             # banned unqualified claims in docs
bash tools/ci_release_version_test.sh      # release versioning discipline (needs pyyaml)
bash tools/compose_scroll_check.sh         # crash guard: nested same-axis vertical scrollables in Compose (self-testing, R25)
bash tools/workflow_yaml_check.sh          # every .github YAML: syntax + duplicate keys (GitHub rejects what PyYAML accepts; self-testing, R25)
bash tools/ci_syntax_mirror_test.sh        # runs the EXACT ci.yml gcc/clang syntax commands (parsed at run time) so local/CI can't drift apart (self-testing, R25)

# 5. Web console (if UI touched)
npm ci && npm run lint                     # ESLint (dev-only dependency)

# 6. ARM/NEON parity — only when an aarch64 cross toolchain + qemu exist:
#    aarch64-linux-gnu-gcc-12 -O2 -static -Iinclude src/hydra_engine.c
#        tests/test_engine.c -lm -o hydra-test-arm && qemu-aarch64-static ./hydra-test-arm
#    plus the x86-vs-ARM token-stream comparison in the skill. Without the
#    toolchain: report "blocked, not measured" (R32). Never a green.

# 7. Android — only with NDK/SDK present (skill: android-ndk-build):
#    gradle assembleDebug, apksigner verify, llvm-nm symbol check, emulator
#    run with logcat evidence (R23). Without it: the static gates above are
#    the maximum honest claim.
```

**Determinism discipline (R22/R24):** all seeds are the LCG
`(state * 1103515245 + 12345) & 0x7FFFFFFF`; the reference token stream for
the starter model is pinned in the skills (host and Android must emit the
same tokens). Cross-platform claims require running both platforms.

## 4. How to work here (the short version)

1. `session.md` → `errors.md` → `rules.md` → write your plan into
   `session.md` (R1). No code before that.
2. Make the change. Additive and honest: fix the rule that was wrong, not the
   document that described it, and never delete a working path to hide a
   problem (this repository's history is full of "removed dead code that was
   the only proof" — errors.md: "Layer aggregation silently deleted the NEON
   kernel").
3. Every new gate ships with a negative control (R25). Every new expectation
   that changed ships with the old value kept as a symptom (R10).
4. Run the gates (§3) until green. Update `session.md` (done/open) and
   `errors.md` (every error hit), and both go into the commit (R4).
5. One logical change per PR, English, open for the user to merge (R17/R18/R31).
   Never merge, never close, never `git checkout` (R17/R20).

## 5. Where things are documented

| Topic | Document |
|---|---|
| v1 binary format + validation rules | `docs/FORMAT.md` |
| `.hydra2` container (specification only, no loader) | `docs/FORMAT-V2.md` |
| What GGUF import actually does | `docs/GGUF-IMPORT.md` |
| Engine architecture + memory model | `docs/ARCHITECTURE.md` |
| v2 research brief | `docs/HYDRA2-RESEARCH.md` |
| GPU feasibility | `docs/gpu-feasibility.md` |
| Android build/verify (long form) | `docs/ANDROID_SKILL.md` |
| Android audit trail | `docs/ANDROID-AUDIT.md` |
| Every error ever hit | `errors.md` |
| Session-by-session history | `session.md` |
| Crash root causes, CI decisions, research with follow-ups (permanent record) | `.agent_memory/RESEARCH.md` |
| Sandbox/tool environments per AI agent (each agent updates its own section only) | `Environment.md` |
| Code-scanning alert handling (event-driven, nothing hardcoded) | `.github/workflows/code-scanning.yml` |

`.agent_memory/RESEARCH.md` is **in the repository on purpose** (owner
decision 2026-10-26; the old gitignore rule that kept it local was removed).
It is the permanent evidence trail for crash root causes, CI architecture
decisions and research whose obligations outlive the session. **Keep it
current:** every non-trivial investigation appends one section (date,
question, method, finding, resolution, pointer to the shipped artifact)
before the session ends. Do not treat it as scratch and do not delete it.

## 6. Missing tools — install them, do not give up
**Environment.md is the shared memory for this.** Before claiming a
tool is missing, read the section that matches your sandbox: another
agent may have recorded a *tested* install path (and the exact error
of every failed one). Afterward you record your own findings — **in
your own section only, never editing another agent's** — with date,
exact command, exact error, measured reason and the workaround, if
any. A check that *can* run locally in your environment **must run
locally** before it is left to CI; "CI will catch it" is not a
record. (Owner rule, 2026-10-26.)


If a gate needs a tool that is absent, **try to install it** before reporting
"blocked" (R32 — a "blocked" claim must say what was attempted and why it
failed). In the Arena sandbox the reachable package channels are
`registry.npmjs.org` and `pypi.org` (both verified working 2026-10-26); the
APT sources (`deb.debian.org`) are **not** reachable there, so
`apt-get install` and therefore **Docker cannot be installed in that
environment** — a container runtime is not required for any gate in this
repository, and nothing here should start needing one. The proven local
ARM toolchain substitute is zig: `python3 -m venv /tmp/zvenv &&
/tmp/zvenv/bin/pip install ziglang`, then `zig cc -target aarch64-linux-gnu`
(see RESEARCH.md §5 for the full inventory and the exact failures). A
"blocked, not measured" verdict is acceptable only after a real attempt was
recorded in `.agent_memory/RESEARCH.md` — never as a default.

## 7. Every crash gets a regression test (crash protocol)

A crash — user report, CI failure, emulator logcat — is not done when the
symptom disappears. It is done when it **cannot return unnoticed**:

1. **Reproduce or reconstruct** the crash on the pre-fix source (git history
   is available even in shallow clones; `git show <sha>:<path>`).
2. **Find the invariant that was violated**, not the line that threw — and
   prove the root cause against the framework/library source when the
   library is open source (fetch the pinned version; line-mapping is
   cheap and ends guessing).
3. **Ship a detector or test that fails on the pre-fix source and passes on
   the fix** — fixture-based for structural classes (see
   `tools/compose_scroll_check.sh` + `tests/fixtures/compose/` as the
   reference: 4 bad + 2 good fixtures, negative control R25, CI gate),
   unit test for behavioral ones.
4. **Wire the detector into CI as a gate** so the class of bug is blocked
   for every future commit, and write the root cause + line map + shipped
   artifacts into `.agent_memory/RESEARCH.md`.

No crash section in RESEARCH.md without its regression test, and no
regression test without a pre-fix red run recorded in the section.

## 8. Issues: how they get closed

- **Feature/fix issues:** GitHub-native — `Closes #N` (or `Fixes #N`) in the
  PR body. This is the default and needs no workflow.
- **Code scanning alerts (the automated channel):** CI reacts to code
  scanning results — **not to issues** (owner rule 2026-10-26; the earlier
  issue-mapping workflow was removed for exactly this reason, and
  **no results may be hardcoded** in any workflow).
  `.github/workflows/code-scanning.yml` triggers on the
  `code_scanning_alert` event and delivers the finding where the author
  can act — PR comment when the alert's commit belongs to an open PR,
  job summary otherwise. Everything it reports comes from the event
  payload at run time. The alert **closes itself**: GitHub resolves it as
  *fixed* when the next analysis (triggered by the fix commit) no longer
  reports it. No manual closing, no issue, no state to manage.
- **Open-ended "check for problems" requests** (like GitHub issue #52,
  auto-closed 2026-10-26 by its proving run): answer them in the
  conversation/PR with the findings, and let the PR close the issue with
  `Closes #N`. Do not build new mapping machinery for a one-off request.
