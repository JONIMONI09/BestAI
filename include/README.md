# `include/` — the public C API (three headers)

| Header | Contract |
|---|---|
| `hydra_model.h` | v1–v4: the 24-byte header struct, format constants, the `HydraEngine` context, the sampler config (`hydra_sampler_config_t` — the single source of sampler ranges for the C header, the JNI header and `server.js`; enforced by `tools/sampler_contract_check.sh`), the zero-heap transformer primitives and the streaming `hydra_kv_cache_t` |
| `hydra_v2_container.h` | `.hydra2` (FORMAT-V2.md): header/tensor-entry structures, the element-type ids (incl. the resident `Q4_0` 0x0030, round 16), roles, the metadata API, error codes. Structural reader — no inference |
| `hydra_moe.h` | `.hydra2` MoE engine (FORMAT-V2.md): `hydra_moe_config_t` (the enforced `v2.*` inference keys), the resolved trunk tensors (F32 **or** raw Q4_0 blocks — never materialized), the stateless forward step, the real per-vocabulary logit vector, the BPE codec. Expert layouts 0/1/2 |

These headers are the API boundary: `android/app/src/main/cpp/hydra_jni.c`
compiles against `hydra_model.h` only (the Android app deliberately does
not link the MoE engine), and the web console never includes any header at
all — it runs `hydra-run` as a child process.
