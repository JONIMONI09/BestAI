# Audit of issue #72 — evidence-based verdict (2026-10-27)

Issue #72 ("post-`fd1848ac` transformer + `.hydra2` expansion", 6
milestones M1–M5) was drafted **before PR #70 was merged**. This
document verifies every claim against the merged code at
`94f8e08` (merge of PR #70, branch `arena/44edb923-bestai` base).
Every verdict carries the measurement or the file:line evidence.

## Verdict summary

| Claim | Verdict |
|---|---|
| M1.1 add `hydra_v2_container.c` to the NDK CMakeLists | **STALE + wrong path** (below) |
| M1.2 header parity for `hydra_v2_container.h/.c` | **DONE** (PR #70, 28 checks) |
| M1.3 test all new JNI entry points (sampler / transformer / .hydra2) | **MIXED**: sampler DONE; transformer gap REAL; .hydra2 entry point DOES NOT EXIST |
| M2.1 SIMD the ternary mat-vecs (container + transformer) | **DONE for the transformer** (PR #70); container has no data-parallel arithmetic (documented N/A) |
| M2.2 per-group FP16/FP32 block scaling (group 64/128) | **NEW** (only a per-layer u16 scale exists today) |
| M3.1 full GQA + MoE FFN + embedding + lm_head forward pass | **NEW model class** — none of these exist in v4 (fixed-design single-head) |
| M3.2 mmap for .hydra2 weight planes | **NEW** (container explicitly does not map the trunk — documented scope) |
| M4 embedded vocab + BPE `--text-prompt` | **NEW** (no format stores token strings today) |
| M5 web console `--temp/--min-p/--rep-penalty` + docs | **DONE** (PR #70); `.hydra2` inspection in console = **NEW** (small) |
| Baseline invariant `[471, 386, 385, 386, 385, 386, 385, 386]` (start_token=42, greedy) | **CONFIRMED by measurement** |

## M1 — Android NDK link, header guardrails, JNI tests

**M1.1 is unnecessary, and its path does not resolve.**

1. `android/app/src/main/cpp/CMakeLists.txt` computes the repo root
   as **5 levels up**:
   `get_filename_component(HYDRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)`
   (comment: "android/app/src/main/cpp -> 5 levels up = repo root").
   The issue's `../../../../src/hydra_v2_container.c` (4 levels up)
   would resolve to `android/src/hydra_v2_container.c` — a
   nonexistent path. CMake would fail with a source-not-found error.
2. Even with the correct path, the addition is dead weight:
   `libhydra.so` compiles exactly two files —
   `${HYDRA_ROOT}/src/hydra_engine.c` + `hydra_jni.c` — and
   **neither references a single container symbol**:
   `hydra_jni.c` includes only `jni.h, android/log.h, stdio.h,
   stdlib.h, time.h, math.h, hydra_model.h, hydra_batch.h,
   hydra_sample_cfg.h`; the only `hydra2` string in
   `src/hydra_engine.c` is a refusal message
   (`src/hydra_engine.c:375`). Linking the container into the phone
   `.so` would add ~40 KB of code no caller can reach.
3. The container is deliberately **host-only**: `docs/FORMAT-V2.md`
   "Implementation status" states "there is no code path in
   `hydra-run` that consumes a `.hydra2` file"; the header
   (`include/hydra_v2_container.h:10-13`) restates the scope: "it is
   a container. It does not run MoE inference, it does not implement
   an expert cache, and it does not map the trunk."

**M1.2 is done (PR #70).** `tools/android_header_check.sh` validates
`include/hydra_v2_container.h` in parallel with `include/hydra_model.h`
— 28 checks incl. two *computed* cross-surface pins (python3
recomputes `HYDRA2_TAIL_U32 = 0x00003241L` and the v1 magic u32 from
the C magic strings) and a negative control (wrong tail → rc=1).
Measured post-merge: `ANDROID-HEADER-CHECK: 28 checks passed`.

**M1.3 is mixed.**

* Sampler config: **done** (PR #70). `tests/test_jni_batch.c`
  109 checks incl. the sampler section (boundaries, 14 rejections
  incl. NaN/±Inf/first-invalid-wins, pinned defaults);
  `tools/jni_signature_negative_test.sh` proves the signature gate
  fails (phantom method + load-descriptor drift), wired into CI for
  both bridges (JDK 17). Measured: `109 checks, 0 failed`.
* Transformer forward pass via JNI: **real gap**. `hydra_jni.c:319`
  documents that `runInferenceSampled` dispatches v3/v4
  ("greedy, v3 → hydra_engine_step_tf() + caller-owned KV cache"),
  and `HydraBridge.kt` documents that `runInference` "still uses the
  v1/v2 step and refuses v3" — but `grep -c "v3\|transformer"
  tests/test_jni_batch.c` = **0**: the batch test never drives a v3
  or v4 model through the JNI surface. The engine-level v3/v4
  semantics ARE pinned (`tests/test_engine.c` 225), so this is a
  bridge-level coverage gap, not a functional one.
* ".hydra2 loading" via JNI: **does not exist**. The bridge exports
  exactly four entry points (`cancel`, `runInference`,
  `runInferenceSampled`, `benchmark` — `hydra_jni.c` L31/42/166/345/665)
  and no `load()`; there is nothing to test. The issue asks for tests
  of a feature that was never built.

## M2 — SIMD for the .hydra2 / block-scaled mat-vec

**M2.1 is done for the only data-parallel hot path.** The v3/v4
projection core `tf_row_dot` (384 calls/token at dim 64) dispatches to
AVX2 (`src/hydra_simd.h`) and NEON (`src/hydra_neon.h`) kernels with a
scalar fallback (PR #70). Both are integer-exact (lane bound 2·127,
total bound 64·127 = 8128) → bit-identical streams; parity tests in
`tests/test_engine.c` (x86 225 / ARM 234). The `.hydra2` container
itself has **no data-parallel arithmetic to SIMD**: `route_top_k` is
comparison-based selection + double softmax over k logits, and the
tensor/expert reads are `pread` copies. SIMD'ing a `memcpy` or an
argmax over ≤ k elements is not a correctness or a meaningful
performance win; this was a documented scope decision in PR #70
(AGENTS.md §2.1, docs/FORMAT-V2.md SIMD status).

**M2.2 (per-group FP16/FP32 scaling, group 64/128) is new work.**
What exists today: ONE u16 absmean scale **per layer**
(`include/hydra_model.h:33` — "the per-layer scale u16 stores
absmean * 256", applied exactly, u16 × int8 stays integer-exact).
Per-group float scales would change the precision semantics of the
pinned streams, so this must be **additive** (new format version or
new container tensor type with a scale plane), never retro-fitted
into v3/v4. Feasible design: Q8_0-style per-group scale ×
ternary×int8 integer accumulation, fused into `tf_row_dot` per group.
Estimate: format + loader + engine + converter + parity tests = a
full milestone.

## M3 — "complete GQA transformer" + mmap

**M3.1 describes a model class that does not exist.** v4
(`include/hydra_model.h:175-215`, PR #70 docs) is fixed-design:

| v4 today (measured) | M3.1 asks for |
|---|---|
| single **full-dimension** causal attention per layer (one Wq/Wk/Wv, no head split, no `n_heads`/`n_kv_heads` field anywhere in the header) | GQA with `n_heads`/`n_kv_heads` |
| dense ternary SwiGLU FFN (Wg/Wu) | Sparse-MoE FFN |
| `w1` token injection (ternary projection of the token ID — NOT a table) | Token Embedding lookup |
| `W_out` ternary projection = the logit step | separate `lm_head` |

There are **no per-model architecture fields** in the v3/v4 header
(the format is "all fixed by the format, not per-model options",
`hydra_model.h:24-25`). The issue conflates two different things:
the `.hydra2` **container** stores tensor *roles*
(`HYDRA2_ROLE_EMBEDDING/NORM/KV`, `hydra_v2_container.h`) for the
future `moe-transformer-v1` architecture — storage, not inference —
and the executable v3/v4 engine. A GQA/MoE engine on the container is
a new inference stack (FORMAT-V2.md lists "MoE inference on top of
the container" under *Not implemented*).

**M3.2 (mmap for .hydra2) is new work reversing a documented
decision.** The container scope explicitly says it "does not map the
trunk" (`hydra_v2_container.h:10-13`); reads go through
`hydra2_read_tensor()` (trunk, host buffer) and one `pread` + CRC-32
per expert (FORMAT-V2.md 5.1.5). Mmap'ing the trunk (resident
region) with a portable `open`+`pread` fallback is a well-bounded
feature; mmap'ing the streaming expert region would be wrong by
design (experts are never resident, FORMAT-V2.md §6.2).

## M4 — embedded vocabulary + BPE text prompts

**No format in this repository stores token strings.**

* v1/v2/v3/v4 `.hydra` header: `vocab_size` only
  (`hydra_model.h:44`), 24 bytes, no room for strings.
  `tools/gguf_to_hydra.py:324`: "the vocabulary cannot be stored".
* `.hydra2` container: tensor roles TRUNK/ROUTER/EMBEDDING/NORM/KV/
  SHARED_EXPERT — no VOCAB role; the metadata block is validated
  routing K/V (top_k etc., `src/hydra_v2_container.c:385-391`).
* CLI: `--prompt` takes **token IDs only** (`src/main.c:104-109`,
  strict numeric parse); output is token IDs. No `--text-prompt`, no
  BPE encoder, no UTF-8 decode anywhere in `src/`.
* The console's "word → token id" mapping is a server-side
  convenience table for chat, not a format feature.

So M4 = new format extension (a vocab tensor/sidecar + strings), a C
token→text decode, a text→token encoder, and converter support —
again a full milestone. Note: `tools/gguf_minimal_llama.py`
**already exists** (R10) and writes a minimal but structurally real
llama GGUF *with a BPE-style tokenizer* — it proves the
**llama.cpp** pipeline (the app's full-fidelity path), which already
handles real text. The BPE work in M4 would duplicate that only for
the *ternary projection* engine.

## M5 — web console + docs

**Done (PR #70):** `server.js` exposes and strictly validates
`temperature/minP/repetitionPenalty/repWindow` (field-less requests
are bit-identical legacy), the console UI sends them,
`tools/server_test.js` 43/43; `AGENTS.md` (§2.1 v3/v4+SIMD, §3
counts), `README.md` (CLI sampler flags), `CHANGELOG.md`,
`docs/FORMAT-V2.md` (SIMD status) and `tools/test_count_baseline.txt`
(x86 225 / arm 234) are all current.

**New (small-medium):** `.hydra2` inspection in the web console —
`server.js` has no `.hydra2` handling at all (grep: only a comment).
The natural shape: a read-only `/api/models/inspect2?path=…` that
runs the existing host-side container reader and returns the
per-region validation report, mirroring the `.hydra` inspect route.

## The issue's verification list — measured

| Command as written | Result |
|---|---|
| `make clean && make && make test` | fine |
| `python3 tools/gguf_test.py` | fine (101 checks offline) |
| `python3 tools/gguf_minimal_llama.py` (no args) | **usage error** — `--out` is required |
| `python3 tools/gguf_speed.py`, `string_format_check.py` | fine |
| `python3 tools/compose_scroll_guard.py` | exists, but the **wired gate is `tools/compose_scroll_check.sh`** (AGENTS.md §3); both exist, the shell one is the CI one |
| `node tools/server_test.js` | works, but the repo convention is `node --test tools/server_test.js` |
| `node tools/console_train_test.js`, `contrast_check.js` | fine |
| `bash tools/test_count_check.sh` (no args) | **usage FAIL** — requires `<binary> <arch-label>` |
| all other `tools/*.sh` in the list | exist and are the real gates |
| `npm run lint && git status` | fine |

## Bottom line

The issue is a **pre-PR#70 snapshot**: ~40% of it (M1.2, M1.3-sampler,
M2.1-transformer, M5) is already merged and verified; M1.1 and the
".hydra2 JNI tests" ask for things that don't exist / aren't needed
(one of them with a path that can't resolve); the genuinely new work
is M2.2 (block scales), M3.1 (GQA/MoE model class), M3.2 (trunk
mmap), M4 (vocab+BPE) and the small console `.hydra2` inspect — each
a separately scopeable milestone. The measured baseline invariant is
correct and stays: starter model, token 42, greedy, 8 steps →
`[471 386 385 386 385 386 385 386]`.
