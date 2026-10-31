---
name: engine-ci-verify
description: "Run all local Hydra-Stone quality gates in CI-identical order: compilers, analyzers, sanitizers, cppcheck, flawfinder, ESLint, build, the six C test suites (incl. the .hydra2 MoE engine), the Python converter gates, the Node server suite, ARM/NEON cross-path parity check via qemu, runtime smoke test."
---

# Engine CI Verification — Hydra-Stone

Run all local quality gates before committing. These mirror the CI
pipeline (`.github/workflows/ci.yml` + `.github/workflows/lint.yml`);
the canonical command list with comments is `AGENTS.md` §3 — this skill
is the executable form.

## When to use

- Before any commit touching `src/`, `include/`, `tests/`, or `tools/`
- When CI fails — run the exact failing gate locally first
- Always run gate 6 (ARM/NEON) when `src/hydra_engine.c` or
  `src/hydra_neon.h` changed

## Gates (run all, in order — fails fast)

```bash
# 1. gcc strict syntax (CI lint job, gate 1a)
gcc -fsyntax-only -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -Wall -Wextra -Werror -Iinclude \
    src/hydra_engine.c src/main.c tests/test_engine.c

# 2. clang strict syntax (CI lint job, gate 1b, the double review)
clang -fsyntax-only -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -Wall -Wextra -Werror -Iinclude \
    src/hydra_engine.c src/main.c tests/test_engine.c

# 3. cppcheck static analysis (CI lint job, gate 1c — ALL of src/ + tests/)
cppcheck --enable=warning,portability --inline-suppr --std=c99 \
    --platform=unix64 -Iinclude --error-exitcode=2 src/ tests/

# 4. Python gate (CI lint job, gate 1d)
python3 -m py_compile tools/make_model.py
python3 -c "import struct; assert struct.calcsize('<IHHIIII') == 24"

# 5. Build + the full C suite (CI build-test: six binaries, make test-run)
make clean && make && make test        # builds engine + ALL test binaries
make test-run                           # RUNS all six suites:
#   hydra-test (234 x86 / arch-dependent)  jni-batch-test (128)
#   large-model-test (7)  gguf (101, python)  hydra2-test (152)
#   hydra-moe-test (216)  -- the moe fixtures come from
#   tools/make_moe_fixtures.sh (the SINGLE source; the Makefile recipe
#   and the CI sanitizer step both call it — never an inline copy)

# 6. NEON path + scalar/NEON parity (CI job "neon-test").
#    ONE TIME SETUP:
#    sudo apt-get install -y gcc-12-aarch64-linux-gnu qemu-user-static
aarch64-linux-gnu-gcc-12 -O2 -static -Wall -Wextra -Iinclude \
    src/hydra_engine.c tests/test_engine.c -lm -o hydra-test-arm
qemu-aarch64-static ./hydra-test-arm          # expect: "N Tests, 0 failures" (N parsed from the output)

aarch64-linux-gnu-gcc-12 -O2 -static -Iinclude \
    src/hydra_engine.c src/main.c -o hydra-run-arm
python3 tools/make_model.py parity.hydra
./hydra-run parity.hydra 42 8 --json 2>/dev/null > /tmp/x86.json
qemu-aarch64-static ./hydra-run-arm parity.hydra 42 8 --json \
    2>/dev/null > /tmp/arm.json
python3 -c "
import json,sys
x=json.load(open('/tmp/x86.json')); a=json.load(open('/tmp/arm.json'))
print('x86:',x['tokens']); print('arm:',a['tokens'])
sys.exit(0 if x['tokens']==a['tokens'] and x['state']==a['state'] else 1)"

# 7. Runtime smoke test
python3 tools/make_model.py smoke.hydra && ./hydra-run smoke.hydra 42

# 8. Optional build-flag variants (both must stay green)
gcc -O2 -DHYDRA_TOKENV_MASK -Iinclude src/hydra_engine.c tests/test_engine.c \
    -lm -o /tmp/t-mask && /tmp/t-mask          # expect "N Tests, 0 failures" (parse N)
aarch64-linux-gnu-gcc-12 -O2 -static -DHYDRA_DROP_CACHE -Iinclude \
    src/hydra_engine.c tests/test_engine.c -lm -o /tmp/t-drop
qemu-aarch64-static /tmp/t-drop                # expect "M Tests, 0 failures" (parse M)

# 9. Python pipeline gates (CI lint job + build-test)
python3 tools/gguf_test.py --engine ./hydra-run    # 101 checks vs the REAL engine
python3 tools/gguf_to_hydra2_test.py ./hydra-run   # 44 checks (make gguf-conv-test)
python3 tools/string_format_check.py
bash tools/text_prompt_check.sh                   # the CLI BPE gate
bash tools/docs_claim_check.sh                    # banned claims in any .md
bash tools/workflow_yaml_check.sh && bash tools/workflow_lint_check.sh
bash tools/session_start_check.sh                 # the R33 marker gate

# 10. Node gates (CI lint job / Lint & Security)
node --test tools/server_test.js                  # 57 live HTTP tests
node tools/console_train_test.js                  # trainer vs compiled engine, bit-for-bit
node tools/contrast_check.js                      # WCAG AA, 17 pairs
npm ci && npm run lint                            # ESLint (dev-only dependency)

# 11. Shell guardrails (the full list in AGENTS.md §3):
#     jni_signature_check.sh (+negative), android_autostart_check.sh
#     (+negative), android_header_check.sh, android_crash_check.sh,
#     llama_pin_check.sh (+negative), sampler_contract_check.sh,
#     test_count_check.sh ./hydra-test x86, compose_scroll_check.sh,
#     ci_syntax_mirror_test.sh, ci_release_version_test.sh

rm -f smoke.hydra parity.hydra hydra-run hydra-test hydra-run-arm hydra-test-arm
```

## The local-only hardening gates (beyond CI, same discipline)

These ran in CI in earlier revisions and now stay local (they are strictly
stronger or redundant-with-CI; keep them green anyway):

```bash
# L1. GCC static analyzer
gcc -fanalyzer -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -Wall -Wextra -Iinclude -c src/hydra_engine.c -o /dev/null

# L2. Clang Static Analyzer (one file at a time — -o takes a single output)
for f in src/hydra_engine.c src/main.c tests/test_engine.c; do
  clang --analyze -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
      -Iinclude -Xanalyzer -analyzer-output=text "$f" -o /dev/null
done

# L3. clang-tidy. The curated check list is deliberate: the style checks
#     (identifier-length, magic-numbers, braces) are pure noise here, and
#     cert-err33-c fires on every printf return, which is not an error
#     indicator for stdio.
clang-tidy src/hydra_engine.c src/main.c tests/test_engine.c \
  --checks='-*,clang-analyzer-*,bugprone-*,cert-*,-cert-err33-c,concurrency-*,-concurrency-mt-unsafe' \
  -- -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude

# L4. flawfinder. Level 4 = high/critical. Level 2 reports every fopen,
#     mkstemp and char array — pure false positives in this codebase.
flawfinder -m 4 --error-level=4 -C -Q src/ tests/ android/app/src/main/cpp/

# L5. ASan + UBSan — the gate that actually reports arithmetic UB.
#     VERBATIM CI SHAPE (build-test sanitizer step): five binaries, the
#     pyfix container via make_model.py --v2, and the moe fixture list
#     from tools/make_moe_fixtures.sh (NEVER inline):
CF="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude"
cc $CF -o /tmp/asan-hydra-test src/hydra_engine.c tests/test_engine.c -lm
cc $CF -o /tmp/asan-jni-batch-test tests/test_jni_batch.c src/hydra_engine.c -lm
cc $CF -o /tmp/asan-large-model-test src/hydra_engine.c tests/test_large_model.c -lm
cc $CF -o /tmp/asan-hydra2-test src/hydra_v2_container.c src/hydra_engine.c tests/test_hydra2_container.c -lm
cc $CF -o /tmp/asan-hydra-moe-test src/hydra_moe.c src/hydra_v2_container.c src/hydra_engine.c tests/test_hydra_moe.c -lm
# detect_leaks=1 is Linux-only (AppleClang ships no LSan; on Darwin CI
# exports an empty ASAN_OPTIONS instead — the ASan+UBSan checks stay on):
export UBSAN_OPTIONS=halt_on_error=1
export ASAN_OPTIONS=detect_leaks=1
/tmp/asan-hydra-test
/tmp/asan-jni-batch-test
/tmp/asan-large-model-test
python3 tools/make_model.py .asan-pyfix.hydra2 --v2
/tmp/asan-hydra2-test "$PWD/.asan-pyfix.hydra2"
tools/make_moe_fixtures.sh "$PWD/.asan-moefix" | tee .asan-moefix-args
/tmp/asan-hydra-moe-test $(tail -n 1 .asan-moefix-args)
rm -f .asan-pyfix.hydra2 .asan-moefix-args .asan-moefix*.hydra2

# L6/L7. Web console ESLint + Android lint ("No issues found") are the
#     CI "Lint & Security" jobs — see AGENTS.md §3 gates 4/5 and the
#     android-ndk-build skill.
```

Toolchain install for L1–L4 (once per machine):
`sudo apt-get install -y clang clang-tidy clang-tools flawfinder cppcheck gcc-12-aarch64-linux-gnu qemu-user-static`

## Expected result

- Gates 1-4: silent, exit 0
- Gate 5: `make test-run` runs six suites, every one "N tests, 0
  failures" (234/128/7/101/152/216 on x86-64). **Do not hard-code N** in
  your checks — parse it from the output; the count differs per
  architecture (the NEON comparison tests are compiled out on x86) and
  drift is gated against `tools/test_count_baseline.txt`.
- Gate 6: `<M> Tests, 0 failures` on ARM, where `M > N` because of the
  NEON-vs-scalar comparison tests, then `x86: [...]` / `arm: [...]` with
  **identical** token and state arrays
- Gate 7: 16 tokens printed + axiom check allows at 1.0, blocks at 0.0
- Gates 9-11: each "N checks, 0 failed" (parsed, never assumed)
- L1–L4: no output, exit 0
- L5: all five binaries exit 0 with no sanitizer diagnostics
- L6/L7: ESLint clean, Android Lint "No issues found"

## Notes

- `-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE` are mandatory — without
  them `madvise`/`O_CLOEXEC`/`clock_gettime` are undeclared under
  strict C99 (real first-run failure, fixed in the CI design).
- Gate 6 is **not optional** for engine changes. The NEON kernel shipped
  with every ternary weight sign-inverted, because each code path had
  only ever been checked against fixed expectations, never against each
  other. `test_neon_matches_scalar` is the structural fix; run it on ARM
  before every commit that touches `src/`.
- L5 (ASan + UBSan) is the only gate that reports *arithmetic* undefined
  behaviour. Every static analyzer stayed silent about the layer-overflow
  bug; UBSan reported `signed integer overflow: 2147483640 + 127 cannot be
  represented in type 'int'` on the reverted code. Never skip it.
- **The fixture list is a script, not a copy.** `tools/make_moe_fixtures.sh`
  is the single source of the nine `hydra-moe-test` fixtures — the
  Makefile and CI both call it (the round-16.5 incident: an inline copy in
  ci.yml drifted and main went red while local stayed green — see
  `.agents/errors.md` 2026-10-31 and `.agents/docs/RESEARCH-LOG.md` §11).
- **Before adding any new gate, prove it can fail**: revert the fix it
  should catch and confirm the gate goes red. A gate that has never gone
  red is decoration (rule R25).
- `-DHYDRA_TOKENV_MASK` re-introduces the 0x7F token mask (only 128 of up
  to 1024 token IDs stay distinguishable) and flips one test assertion
  accordingly, so that build legitimately reports a **different** count.
  Parse it; do not compare it to a stored number.
- macOS: cppcheck gate runs on ubuntu only in CI; the clang-ARM64 build
  covers the NEON path there as well.
