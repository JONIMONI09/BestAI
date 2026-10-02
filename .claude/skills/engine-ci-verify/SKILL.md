---
name: engine-ci-verify
description: "Run all local Hydra-Stone quality gates in CI-identical order: gcc and clang strict syntax, cppcheck, python format gate, build, 49 unit tests, ARM/NEON cross-path parity check via qemu, runtime smoke test."
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
qemu-aarch64-static /tmp/t-drop                # expect: 49 Tests, 0 Fehler

rm -f smoke.hydra parity.hydra hydra-run hydra-test hydra-run-arm hydra-test-arm
```

## Expected result

- Gates 1-4: silent, exit 0
- Gate 5: `47 Tests, 0 Fehler` (x86 scalar path; the 2 NEON comparison
  tests are compiled out there)
- Gate 6: `49 Tests, 0 Fehler` on ARM (the 2 extra tests are the
  NEON-vs-scalar comparison), then `x86: [...]` / `arm: [...]` with
  **identical** token and state arrays
- Gate 7: 16 tokens printed + axiom check allows at 1.0, blocks at 0.0

## Notes

- `-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE` are mandatory — without
  them `madvise`/`O_CLOEXEC`/`clock_gettime` are undeclared under
  strict C99 (real first-run failure, fixed in the CI design).
- Gate 6 is **not optional** for engine changes. The NEON kernel shipped
  with every ternary weight sign-inverted, because each code path had
  only ever been checked against fixed expectations, never against each
  other. `test_neon_matches_scalar` is the structural fix; run it on ARM
  before every commit that touches `src/`.
- `-DHYDRA_TOKENV_MASK` re-introduces the 0x7F token mask (only 128 of up
  to 1024 token IDs stay distinguishable) and flips one test assertion
  accordingly, hence 46 instead of 47 tests.
- macOS: cppcheck gate runs on ubuntu only in CI; the clang-ARM64 build
  covers the NEON path there as well.