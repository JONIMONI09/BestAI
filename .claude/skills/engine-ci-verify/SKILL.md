---
name: engine-ci-verify
description: "Run all local Hydra-Stone quality gates in CI-identical order: gcc and clang strict syntax, cppcheck, python format gate, build, 32 unit tests, runtime smoke test."
---

# Engine CI Verification — Hydra-Stone

Run all local quality gates before committing. These mirror the CI
pipeline (`.github/workflows/ci.yml`) exactly.

## When to use

- Before any commit touching `src/`, `include/`, `tests/`, or `tools/`
- When CI fails — run the exact failing gate locally first

## Gates (run all, in order — fails fast)

```bash
# 1. gcc strict syntax (CI gate 1a)
gcc -fsyntax-only -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -Wall -Wextra -Werror -Iinclude src/hydra_engine.c src/main.c

# 2. clang strict syntax (CI gate 1b — the double review)
clang -fsyntax-only -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -Wall -Wextra -Werror -Iinclude src/hydra_engine.c src/main.c

# 3. cppcheck static analysis (CI gate 1c)
cppcheck --enable=warning,portability --inline-suppr --std=c99 \
    --platform=unix64 -Iinclude --error-exitcode=2 src/ tests/

# 4. Python gate (CI gate 1d)
python3 -m py_compile tools/make_dummy_model.py
python3 -c "import struct; assert struct.calcsize('<IHHIIII>') == 24"

# 5. Build + full test suite
make all && make test && ./hydra-test

# 6. Runtime smoke test
python3 tools/make_dummy_model.py smoke.hydra && ./hydra-run smoke.hydra 42
rm -f smoke.hydra hydra-run hydra-test
```

## Expected result

- Gates 1–4: silent, exit 0
- Gate 5: `32 Tests, 0 Fehler`
- Gate 6: 16 tokens printed + axiom check allows at 1.0, blocks at 0.0

## Notes

- `-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE` are mandatory — without
  them `madvise`/`O_CLOEXEC`/`clock_gettime` are undeclared under
  strict C99 (real first-run failure, fixed in the CI design).
- macOS: cppcheck gate runs on ubuntu only in CI; the clang-ARM64 build
  covers the NEON path there instead.
