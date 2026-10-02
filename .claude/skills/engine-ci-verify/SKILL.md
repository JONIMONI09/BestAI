---
name: engine-ci-verify
description: "Run all local Hydra-Stone quality gates in CI-identical order: compilers, analyzers, sanitizers, cppcheck, flawfinder, ESLint, build, 49 unit tests, ARM/NEON cross-path parity check via qemu, runtime smoke test."
---

# Engine CI Verification — Hydra-Stone

Run all local quality gates before committing. These mirror the CI
pipeline (`.github/workflows/ci.yml`) exactly.

## When to use

- Before any commit touching `src/`, `include/`, `tests/`, or `tools/`
- When CI fails — run the exact failing gate locally first
- Always run gate 6 (ARM/NEON) when `src/hydra_engine.c` or
  `src/hydra_neon.h` changed

## Gates (run all, in order — fails fast)

```bash
# 1. gcc strict syntax (CI gate 1a)
gcc -fsyntax-only -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -Wall -Wextra -Werror -Iinclude \
    src/hydra_engine.c src/main.c tests/test_engine.c

# 2. clang strict syntax (CI gate 1b, the double review)
clang -fsyntax-only -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -Wall -Wextra -Werror -Iinclude \
    src/hydra_engine.c src/main.c tests/test_engine.c

# 3. cppcheck static analysis (CI gate 1c)
cppcheck --enable=warning,portability --inline-suppr --std=c99 \
    --platform=unix64 -Iinclude --error-exitcode=2 src/ tests/

# 4. Python gate (CI gate 1d)
python3 -m py_compile tools/make_dummy_model.py
python3 -c "import struct; assert struct.calcsize('<IHHIIII') == 24"

# 5. Build + full test suite (scalar path)
make all && make test && ./hydra-test

# 6. NEON path + scalar/NEON parity (CI job "neon-test").
#    ONE TIME SETUP:
#    sudo apt-get install -y gcc-12-aarch64-linux-gnu qemu-user-static
aarch64-linux-gnu-gcc-12 -O2 -static -Wall -Wextra -Iinclude \
    src/hydra_engine.c tests/test_engine.c -lm -o hydra-test-arm
qemu-aarch64-static ./hydra-test-arm          # expect: 49 Tests, 0 Fehler

aarch64-linux-gnu-gcc-12 -O2 -static -Iinclude \
    src/hydra_engine.c src/main.c -o hydra-run-arm
python3 tools/make_dummy_model.py parity.hydra
./hydra-run parity.hydra 42 8 --json 2>/dev/null > /tmp/x86.json
qemu-aarch64-static ./hydra-run-arm parity.hydra 42 8 --json \
    2>/dev/null > /tmp/arm.json
python3 -c "
import json,sys
x=json.load(open('/tmp/x86.json')); a=json.load(open('/tmp/arm.json'))
print('x86:',x['tokens']); print('arm:',a['tokens'])
sys.exit(0 if x['tokens']==a['tokens'] and x['state']==a['state'] else 1)"

# 7. Runtime smoke test
python3 tools/make_dummy_model.py smoke.hydra && ./hydra-run smoke.hydra 42

# 8. Optional build-flag variants (both must stay green)
gcc -O2 -DHYDRA_TOKENV_MASK -Iinclude src/hydra_engine.c tests/test_engine.c \
    -lm -o /tmp/t-mask && /tmp/t-mask          # expect: 46 Tests, 0 Fehler
aarch64-linux-gnu-gcc-12 -O2 -static -DHYDRA_DROP_CACHE -Iinclude \
    src/hydra_engine.c tests/test_engine.c -lm -o /tmp/t-drop
qemu-aarch64-static /tmp/t-drop                # expect: 51 Tests, 0 Fehler

rm -f smoke.hydra parity.hydra hydra-run hydra-test hydra-run-arm hydra-test-arm
```

The `.github/workflows/lint.yml` pipeline adds these further gates (run them
locally before touching engine code — see the note below on why the
sanitizer one is not optional):

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

# L5. ASan + UBSan. THE gate that actually reports arithmetic UB.
gcc -O1 -g -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -Wall -Wextra -Iinclude \
    src/hydra_engine.c tests/test_engine.c -lm -o /tmp/hydra-san
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 /tmp/hydra-san

# L6. Web console (ESLint, dev-only dependency)
npm ci && npm run lint

# L7. Android Lint — must report "No issues found"
cd android && /opt/gradle-8.7/bin/gradle lintDebug --no-daemon -q
grep -q "No issues found" app/build/reports/lint-results-debug.txt
```

Toolchain install for L1–L5 (once per machine):
`sudo apt-get install -y clang clang-tidy clang-tools flawfinder cppcheck gcc-12-aarch64-linux-gnu qemu-user-static`

## Expected result

- Gates 1-4: silent, exit 0
- Gate 5: `49 Tests, 0 Fehler` (x86 scalar path; the 2 NEON comparison
  tests are compiled out there)
- Gate 6: `51 Tests, 0 Fehler` on ARM (the 2 extra tests are the
  NEON-vs-scalar comparison), then `x86: [...]` / `arm: [...]` with
  **identical** token and state arrays
- Gate 7: 16 tokens printed + axiom check allows at 1.0, blocks at 0.0
- L1–L4, L6: no output, exit 0
- L5: `49 Tests, 0 Fehler` and no sanitizer diagnostics
- L7: Android Lint "No issues found"

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
- **Before adding any new gate, prove it can fail**: revert the fix it
  should catch and confirm the gate goes red. A gate that has never gone
  red is decoration (rule R25).
- `-DHYDRA_TOKENV_MASK` re-introduces the 0x7F token mask (only 128 of up
  to 1024 token IDs stay distinguishable) and flips one test assertion
  accordingly, hence 46 instead of 47 tests.
- macOS: cppcheck gate runs on ubuntu only in CI; the clang-ARM64 build
  covers the NEON path there as well.