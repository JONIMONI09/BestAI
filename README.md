# Hydra-Stone

**Zero-RAM, ternary O(1) inference engine — drop in any `.hydra` model and go.**

```bash
./hydra-run my_model.hydra 123
```

Hydra-Stone is a C99 engine that streams ternary weights ({-1, 0, +1}) directly from disk via `mmap`. The inference step consists purely of integer additions.

**Memory claim, stated honestly:** the engine performs **no heap allocation at all** — the only process memory it uses is the O(1) `int8_t[64]` state vector. The weight pages themselves are owned by the kernel page cache and are reclaimed under memory pressure by LRU eviction. The engine cannot and does not keep the model's resident set size at zero; build with `-DHYDRA_DROP_CACHE` to explicitly hand touched pages back after every step.

---

## Why It's Fast

| Approach | Multiplications/Token | RAM Growth | Typical Bottleneck |
|---|---|---|---|
| FP32 Transformer | O(N² · d) | KV cache grows unbounded | HBM bandwidth |
| INT8 quantization | O(N² · d) | KV cache grows unbounded | Dequantization overhead |
| **Hydra-Stone (ternary + mmap)** | **0** (addition only) | **none (O(1) state)** | disk/cache bandwidth only |

Three levers:

1. **Ternary weights** — no multiplication needed: `y = Σ_{w=+1} x − Σ_{w=−1} x`
2. **mmap paging** — the kernel faults in only the 4-KiB pages the compute cursor actually touches; clean pages are reclaimable under memory pressure (no swap traffic).
3. **O(1) recurrent state** — instead of a growing KV cache, a fixed `int8_t[64]` vector carries the state.

### A bug worth reading about

Until 2026-10-02 the NEON kernel decoded the ternary code `01` as **−1** instead of +1: `vceqq_u8` returns `0xFF` on a match, and `vsubq_s8(p, n)` therefore computed `0xFF − 0x00 = −1`. ARM produced **exactly negated weights** and silently different token sequences than x86, while the documentation claimed both paths were bit-identical.

It survived because *no test compared the two paths to each other* — every test compared the engine against fixed expectations, and each platform only ever ran its own path. The fix (`src/hydra_neon.h`, explicit `0x01`/`0xFF` mask normalisation) came together with `test_neon_matches_scalar`, which now runs a scalar reference **inside the same ARM build** and demands bit-identical tokens *and* state vectors. That test immediately caught a second latent defect in the SIMD store offsets, which would otherwise have shipped.

## Quick Start

```bash
# 1. Compile
make

# 2. Generate a test model
python3 tools/make_dummy_model.py test.hydra

# 3. Feed the model to the engine
./hydra-run test.hydra 123

# 4. Unit tests (32)
make test-run

# 5. Start the web console (desktop-optimized)
make ui    # → http://localhost:8787
```

## Web Console (Desktop)

The **Hydra-Stone Console** is a dark terminal-style desktop UI featuring:

- **Model panel** — dim/vocab/layers of the loaded model
- **Inference controls** — start token & steps; Enter triggers a run
- **Live statistics** — total ms, ms/token, tokens/s
- **Token stream chart** — canvas visualization of the output sequence
- **O(1) state heatmap** — 64 state cells, colored live
- **Coexistence axiom slider** — H(s) from −0.5 to 1.0, Blocked/Allowed in real time

Architecture: `server.js` (Node, **zero npm dependencies**) → `hydra-run --json` (C engine); frontend is plain HTML/CSS/JS under `public/`.

| Endpoint | Method | Description |
|---|---|---|
| `/api/model` | GET | Header info of the model |
| `/api/infer` | POST | `{token, steps}` → JSON with tokens, state, timing |
| `/api/axiom?h=0.5` | GET | Axiom gate simulation |

## Features

- ✅ **Zero-heap inference** — weights are never copied, only mapped; no `malloc` in the inference path
- ✅ **Ternary linear math** — ternary weights, no FP multiplication in the inner loop. *Density note:* the v1 format stores 2 weights per byte, i.e. **4 bits per weight on disk** (true 2-bit packing via 4 weights/byte is planned for v2) — see `docs/FORMAT.md`.
- ✅ **Coexistence axiom** — `humanity ≤ 0 ∨ NaN ∨ ±∞ ⇒ Utility = −∞`, hard safety gate before any action
- ✅ **NEON SIMD kernel** — 16 parallel ternary accumulations, **actively integrated** into `hydra_engine_step()` on ARM (`__ARM_NEON`); bit-identical scalar fallback on x86
  - *Honesty note:* the NEON path is active on ARM builds only. It is **not** merely covered indirectly — `tests/test_engine.c` runs a scalar reference implementation next to the NEON kernel **in the same ARM build** and requires bit-identical token sequences *and* state vectors. The x86 path is purely scalar; AVX2 is on the roadmap.
- ✅ **Hardening** — header validation (little-endian decode, layer cap, offset checks), OOB protection, overflow-free accumulation, 49 unit tests
- ✅ **Web console** — desktop UI with live visualization (`make ui`)
- ✅ **Android** — NDK/JNI build of the same C source, verified on-device
- ✅ **C99, zero dependencies** — runs on 32-bit ARMv7, x86-64, and everything in between

## Project Structure

```
├── include/hydra_model.h      Public API + binary format header
├── src/hydra_engine.c         mmap loader, ternary inference, axiom gate
├── src/main.c                 CLI
├── src/hydra_neon.h           ARM NEON kernel (actively integrated; scalar fallback on x86)
├── tools/make_dummy_model.py  Test model generator (format reference)
├── tests/test_engine.c        49 unit tests (incl. NEON-vs-scalar, OOB + overflow PoC regressions)
├── android/                   NDK/JNI app wrapping the same C source
├── server.js                  UI server (Node, 0 npm dependencies)
├── public/                    Hydra-Stone Console (HTML/CSS/JS)
├── docs/ARCHITECTURE.md       Architecture & math
├── docs/FORMAT.md             .hydra binary format specification
└── LICENSE                    MIT
```

## The Math in 60 Seconds

**Quantization (absmean):**
```
γ = 1 / mean(|W|)          W̃ = clip(round(γ · W), -1, +1)
```

**Forward step without multiplication:**
```
acc_i += (+1) · x_j   for every w_ij = +1
acc_i += (−1) · x_j   for every w_ij = −1
```

**On-disk footprint per parameter (v1): 4 bits** — two ternary weights per byte, upper 4 bits reserved. Measured against a naive FP16 checkpoint:

| Parameters | FP16 | INT4 (4 bits) | Hydra v1 (4 bits) | Hydra v2 (2 bits, planned) |
|---|---|---|---|---|
| 0.5 B | 1000 MiB | 250 MiB | 250 MiB | 125 MiB |
| 1.0 B | 2000 MiB | 500 MiB | 500 MiB | 250 MiB |
| 1.5 B | 3000 MiB (OOM) | 750 MiB | 750 MiB | 375 MiB |

The honest framing: **v1's win over FP16 is compression, not density parity.** It matches INT4 on disk and beats it in *compute* (additions only, no dequantization, no FP unit). The 2× edge over INT4 arrives with 4-weights-per-byte packing in v2 — see `docs/FORMAT.md`.

## Testing Strategy

The test suite goes beyond smoke checks — every test can fail:

- **Hand-computed decoder cases** — known weight bytes with hand-derived expected tokens (e.g. `w=0x01, token=5 → 11`); any packing bit-order or token-derivation error fails immediately.
- **NEON vs. scalar, same build** — on ARM, a scalar reference implementation of `step()` runs alongside the SIMD kernel over the same model; token sequences **and** state vectors must be bit-identical. This is what caught the NEON sign inversion described below.
- **Seeded roundtrip & variance** — 5 deterministic LCG seeds generate random ternary weight patterns; per seed, all 32 outputs must stay within vocab and a second run with a *fresh engine instance* must reproduce the bit-identical sequence. Sequences must differ across seeds (detects a no-op engine).
- **Security regressions** — crafted headers (bad magic, out-of-bounds weights, `layers × dim > weights_len`, `weights_offset` pointing into the header, `layers = 16 909 321` signed-overflow PoC) must be rejected at load. Every one of these was a real, reproduced bug first.
- **Axiom edge cases** — `NaN`, `±Inf` and negative `humanity_factor` must all block (`NaN ≤ 0` is false in IEEE-754, so the finiteness check is load-bearing).
- **Cross-platform determinism** — verified empirically: host x86-64 (scalar), ARM64 under NEON, and the Android emulator all produce the identical token sequence for the same model.

## Roadmap

- [ ] Full transformer forward pass (RMSNorm, RoPE, SwiGLU) on top of the ternary core
- [ ] Min-P sampling & repetition penalty
- [ ] GGUF/safetensors import with automatic absmean ternarization
- [ ] AVX2/AVX-512 LUT kernel for x86 (current x86 path: purely scalar)
- [ ] 4-weights-per-byte packing (true 2 bits/weight) and an aggregated `A[i]/B[i]` model format — see the redundancy analysis in `docs/ARCHITECTURE.md`
- [ ] Streaming ring-buffer KV with attention sinks

Contributions welcome — see `docs/FORMAT.md` for the binary spec and dive in.

## License

MIT — see [LICENSE](LICENSE).
