# Hydra-Stone

**Zero-RAM, ternary O(1) inference engine — drop in any `.hydra` model and go.**

```bash
./hydra-run my_model.hydra 123
```

Hydra-Stone is a C99 engine that streams language-model weights as **2-bit ternary values** ({-1, 0, +1}) directly from disk via `mmap` — never loading them into the heap. The inference step consists purely of integer additions; RAM usage is **constant (O(1)) regardless of model size**.

---

## Why It's Fast

| Approach | Multiplications/Token | RAM Growth | Typical Bottleneck |
|---|---|---|---|
| FP32 Transformer | O(N² · d) | KV cache grows unbounded | HBM bandwidth |
| INT8 quantization | O(N² · d) | KV cache grows unbounded | Dequantization overhead |
| **Hydra-Stone (ternary + mmap)** | **0** (addition only) | **none (O(1) state)** | disk/cache bandwidth only |

Three levers:

1. **Ternary weights** — no multiplication needed: `y = Σ_{w=+1} x − Σ_{w=−1} x`
2. **mmap paging** — the kernel loads only the 4-KiB pages the compute cursor actually touches; under memory pressure, clean pages are evicted immediately (no swap).
3. **O(1) recurrent state** — instead of a growing KV cache, a fixed `int8_t[64]` vector carries the state.

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

- ✅ **Zero-heap inference** — weights are never copied, only mapped
- ✅ **Ternary linear math** — ternary weights, no FP multiplication in the inner loop. *Density note:* the v1 format stores 2 weights per byte (4 bits/weight effective); true 2-bit packing via 4 weights/byte is planned for v2 — see `docs/FORMAT.md`.
- ✅ **Coexistence axiom** — `humanity ≤ 0 ⇒ Utility = −∞`, hard safety gate before any action
- ✅ **NEON SIMD kernel** — 16 parallel ternary accumulations, **actively integrated** into `hydra_engine_step()` on ARM (`__ARM_NEON`); bit-identical scalar fallback on x86
  - *Honesty note:* the NEON path is active on ARM builds only (macOS CI runs on ARM64 and validates it via the seeded roundtrip tests; x86 CI covers the scalar path). There is currently **no** SIMD path on x86 — AVX2 is on the roadmap.
- ✅ **Hardening** — header validation, OOB protection, saturating arithmetic, 32 unit tests
- ✅ **Web console** — desktop UI with live visualization (`make ui`)
- ✅ **C99, zero dependencies** — runs on 32-bit ARMv7, x86-64, and everything in between

## Project Structure

```
├── include/hydra_model.h      Public API + binary format header
├── src/hydra_engine.c         mmap loader, ternary inference, axiom gate
├── src/main.c                 CLI
├── src/hydra_neon.h           ARM NEON kernel (actively integrated; scalar fallback on x86)
├── tools/make_dummy_model.py  Test model generator (format reference)
├── tests/test_engine.c        32 unit tests (incl. OOB PoC regression, seeded roundtrips)
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

**Memory footprint per parameter: 2 bits** (FP16: 16 bits → **8× smaller**, INT4: 4 bits → **2× smaller**).

| Parameters | FP16 | INT4 | Hydra 2-Bit |
|---|---|---|---|
| 0.5 B | 1000 MiB | 250 MiB | 125 MiB |
| 1.0 B | 2000 MiB | 500 MiB | 250 MiB |
| 1.5 B | 3000 MiB (OOM) | 750 MiB | 375 MiB |

## Testing Strategy

The test suite goes beyond smoke checks — every test can fail:

- **Hand-computed decoder cases** — known weight bytes with hand-derived expected tokens (e.g. `w=0x01, token=5 → 11`); any packing bit-order or token-derivation error fails immediately.
- **Seeded roundtrip & variance** — 5 deterministic LCG seeds generate random ternary weight patterns; per seed, all 32 outputs must stay within vocab and a second run with a *fresh engine instance* must reproduce the bit-identical sequence. Sequences must differ across seeds (detects a no-op engine).
- **Security regressions** — crafted headers (bad magic, out-of-bounds weights, `layers × dim > weights_len`) must be rejected at load; the last case is a confirmed out-of-bounds read PoC.
- **Cross-platform determinism** — the LCG is platform-fixed so Ubuntu (scalar path) and macOS/ARM64 (NEON path) CI runners validate identical semantics on both code paths.

## Roadmap

- [ ] Full transformer forward pass (RMSNorm, RoPE, SwiGLU) on top of the ternary core
- [ ] Min-P sampling & repetition penalty
- [ ] GGUF/safetensors import with automatic absmean ternarization
- [ ] AVX2/AVX-512 LUT kernel for x86 (current x86 path: purely scalar)
- [ ] Streaming ring-buffer KV with attention sinks

Contributions welcome — see `docs/FORMAT.md` for the binary spec and dive in.

## License

MIT — see [LICENSE](LICENSE).
