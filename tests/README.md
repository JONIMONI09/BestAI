# `tests/` — the C test suites (all run by `make test-run`)

| Suite | Binary | What it pins |
|---|---|---|
| `test_engine.c` | `hydra-test` (234 checks on x86-64, architecture-dependent) | hand-computed engine cases, NEON-vs-scalar and AVX2-vs-scalar parity, the v3/v4 transformer streams, the sampler suite, OOB/overflow security regressions |
| `test_jni_batch.c` | `jni-batch-test` (128 checks) | the JNI token-batching rule (shares `android/app/src/main/cpp/hydra_batch.h` with the app), v3/v4 batched-vs-direct parity |
| `test_large_model.c` | `large-model-test` (7 checks) | 5 GiB sparse model, 32-bit offset refusals |
| `test_hydra2_container.c` | `hydra2-test` (152 checks) | the `.hydra2` container reader: header/tensor/metadata validation, loud refusals, the role-8 vocab types (FORMAT-V2.md) |
| `test_hydra_moe.c` | `hydra-moe-test` (216 checks) | the MoE engine: reference parity, greedy stream pins per fixture (incl. the Q4_0 layout-2 fixtures), KV eviction, sampler extremes, dequant round-trips, shape-tamper + metadata refusals (FORMAT-V2.md) |

`fixtures/` holds the structural test inputs (currently the Compose
scroll-guard fixtures for `tools/compose_scroll_check.sh`).

**The MoE fixtures are not in this folder:** `tests/test_hydra_moe.c`
requires 9 fixture models as argv, and the single source of that list is
`tools/make_moe_fixtures.sh` (the Makefile `test-run` recipe and the ci.yml
sanitizer step both call it — see `.agents/errors.md`, 2026-10-31).

Counts are parsed from the binary output, never hard-coded; drift is gated
against `tools/test_count_baseline.txt` by `tools/test_count_check.sh`.
Every crash in the project history has a regression test here or in the
shell gates (rules R25/R33 — see `.agents/rules.md`).
