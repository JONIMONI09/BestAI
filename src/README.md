# `src/` — the C engine (one translation unit per concern)

| File | What it is |
|---|---|
| `hydra_engine.c` | The v1–v4 engine: mmap loader (v1–v4 of `docs/FORMAT.md`), the recurrent step + load-time aggregation, the min-p/repetition-penalty sampler, the zero-heap transformer primitives (RMSNorm/RoPE/SwiGLU, `hydra_kv_cache_t`), the v3/v4 per-layer passes (`hydra_engine_step_tf` / `hydra_transformer_forward_step`), and the magic dispatch (`hydra-run` routes `.hydra2` files to the MoE engine instead) |
| `hydra_v2_container.c` | The `.hydra2` container reader (FORMAT-V2.md): 128-byte header, tensor directory, routing + inference metadata, mmap trunk with pread fallback, loud refusals. Structural only — it does not run inference |
| `hydra_moe.c` | The `.hydra2` MoE inference engine (FORMAT-V2.md): GQA attention (block mapping, partial RoPE, per-layer sink+ring KV), deterministic top-k routing, streamed expert FFN in layouts 0 (F32) / 1 (TERNARY2 + block scales) / 2 (Q4_0, row-by-row dequant, 4-bit resident), real per-vocabulary logits, the BPE vocab codec |
| `main.c` | The CLI (`hydra-run`): `--json`, sampler flags, `--prompt` (token IDs) / `--text-prompt` (BPE), `--bench`; dispatches on the file magic |
| `hydra_neon.h` | NEON kernels: the v1/v2 16-lane step chunk **and** the `tf_row_dot` v3/v4 kernel (bit-identical to scalar, parity-pinned) |
| `hydra_simd.h` | AVX2 kernels: `hydra_avx2_accumulate_agg` (step), the v2 plane unpack, the `tf_row_dot` v3/v4 kernel (bit-identical to scalar, parity-pinned) |

Build: `make` (produces `hydra-run` + all test binaries). The engine is
strict C99 — the CI syntax gates run with `-Wall -Wextra -Werror` and the
POSIX feature-test defines (see `.agents/skills/engine-ci-verify/SKILL.md`).
Nothing here is forked for Android: the NDK compiles these same files into
`libhydra.so` (see `android/README.md`).
