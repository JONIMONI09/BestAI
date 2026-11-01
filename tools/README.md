# `tools/` — generators, converters and the local gate scripts

## Generators / converters (Python, stdlib only unless noted)

| Tool | Purpose |
|---|---|
| `make_model.py` | Canonical model writers: v1 starter, v3/v4, the `--v2`/`--moe-full` `.hydra2` fixtures (incl. ternary, GQA, attention-ternary) |
| `make_moe_fixtures.sh` | **The single source of the 9 `hydra-moe-test` fixtures** (`<base>`, `-t64`, `-t128`, `-t64-lg`, `-nv`, `-at64`, `-gqa`, `-q4`, `-q4f32`); prints their paths as the last stdout line — the Makefile `test-run` recipe and the ci.yml sanitizer step both call it (no copy) |
| `build_3b_synth_gguf.py` | The 3B-geometry synthetic GGUF (llama arch, Q4_0 trunk, 11,696-tensor listing) for the opt-in 3B E2E gate (`HYDRA2_3B_E2E=1`) |
| `gguf_reader.py` | GGUF header/tensor reader: six layouts (F32/F16/BF16/Q8_0/Q4_0/Q4_1), C-speed decoders |
| `gguf_inspect.py` | Read-only GGUF report — "is this file convertible?" before a long conversion |
| `gguf_to_hydra.py` | GGUF → `.hydra` (v1–v3), absmean ternarisation |
| `gguf_to_hydra2.py` | GGUF → `.hydra2` (full-fidelity MoE model + embedded BPE vocab, 32-chunk streaming; `--t4` resident Q4_0 output, `--probe` Q4_0-clean report; an optional numpy fast path is byte-identical to the stdlib reference) |
| `safetensors_to_hydra.py` | safetensors → `.hydra` converter (independent of the GGUF path) |
| `gguf_minimal_llama.py` | Minimal structurally-real llama GGUF writer (fixed seed) — input of the pinned llama.cpp smoke test (`--dtype q40/q41` since round 15) |
| `hydra_train.js` | The web trainer — a line-by-line mirror of `hydra_engine_step()`, hill-climbing on the whole-sample score |

## Gates (the local equivalents of the CI jobs)

| Tool | What it gates |
|---|---|
| `gguf_test.py` (101 checks) | the v1 converter against the real C engine (opt-in real-model gate) |
| `gguf_to_hydra2_test.py` (44 checks) | the `.hydra2` converter: engine stream, F32-exact re-reads, quantiser parity, refusal controls (`make gguf-conv-test`) |
| `server_test.js` (57 checks) | the web console API over live HTTP, each check with a negative control |
| `console_train_test.js` | trainer vs the compiled engine, bit-for-bit |
| `contrast_check.js` | WCAG AA contrast in `public/` (17 pairs) |
| `test_count_check.sh` | parsed test count vs `test_count_baseline.txt` (count drift is a red gate) |
| `sampler_contract_check.sh` | C header ↔ JNI header ↔ server.js sampler contract (the C header is the single source) |
| `jni_signature_check.sh` (+ `_negative_test.sh`) | Kotlin ↔ C JNI signatures (SKIPs without javac — say so) |
| `android_autostart_check.sh` (+ negative), `android_header_check.sh`, `android_crash_check.sh` | the Android app's invariants (autostart, header rules, crash-report chain) |
| `compose_scroll_check.sh` + `fixtures/compose/` | the Compose nested-scroll crash guard (4 bad + 2 good fixtures) |
| `llama_pin_check.sh` (+ negative) | the llama.cpp submodule pin + MIT licence |
| `docs_claim_check.sh` | banned unqualified capability claims in any `.md` |
| `string_format_check.py` (+ negative test) | printf format-string/specifier contract |
| `ci_syntax_mirror_test.sh` | runs the EXACT ci.yml gcc/clang syntax commands — local may never drift from CI |
| `workflow_yaml_check.sh`, `ci_release_version_test.sh` | GitHub-workflow YAML sanity (duplicate keys, bad triggers) + release versioning discipline |
| `session_start_check.sh` | the R33 session-start marker rule |
| `fetch_test_model.sh`, `gguf_fixture.js`, `gguf_speed.py` | real-model fetch helper, the JS GGUF fixture writer, converter throughput measurement |
| `gpu_bench/` | the (NOT-MEASURED) GPU feasibility benchmark — own README |
| `kotlin_stub_check/` | Kotlin type-check without the Android SDK — own README |

Run everything: `make test-run` (C suites), then the Python/Node gates in
`AGENTS.md` §3.
