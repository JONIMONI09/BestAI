# Hydra-Stone Architecture

## Overview

```
┌────────────────────────────────────────────────────────────┐
│                      hydra-run (CLI)                       │
├────────────────────────────────────────────────────────────┤
│  hydra_engine_load()  →  mmap() + header validation        │
│  hydra_engine_step()  →  ternary O(1) inference            │
│  hydra_verify_axiom() →  coexistence axiom (safety gate)   │
├────────────────────────────────────────────────────────────┤
│                 .hydra model format (v1)                   │
│  [24B header][2-bit ternary weights, packed]               │
└────────────────────────────────────────────────────────────┘
```

## Components

| File | Purpose |
|---|---|
| `include/hydra_model.h` | Public API, header struct, defines |
| `src/hydra_engine.c` | Loader (mmap), inference step, axiom gate |
| `src/main.c` | CLI entry point (`--json` mode for machine-readable output) |
| `src/hydra_neon.h` | NEON SIMD kernel — **actively integrated**: `step()` uses it on ARM in 16-lane chunks; bit-identical scalar fallback for remainders and x86 |
| `tools/make_dummy_model.py` | Test model generator (reference implementation of the format) |
| `tests/test_engine.c` | 32 unit tests (hand-computed cases, seeded roundtrips, security regressions) |
| `server.js` | Web console server (Node, zero npm dependencies) |
| `public/` | Hydra-Stone Console frontend (plain HTML/CSS/JS) |

## Memory Model: Zero-RAM via mmap

1. Weights are **never** copied into the heap.
2. `mmap(PROT_READ, MAP_SHARED)` maps the file into the address space.
3. `madvise(MADV_SEQUENTIAL)` tells the kernel the access pattern → aggressive read-ahead.
4. 4-KiB pages are loaded on demand and evicted immediately under memory pressure (clean pages → no swap needed).
5. The runtime state is a **fixed** `int8_t[64]` vector → O(1) RAM.

**Consequence:** inference-time RAM usage is constant regardless of model size. A 1-GB model consumes no heap — only page cache, which the kernel evicts as needed.

## Ternary Math

Weights: `W ∈ {-1, 0, +1}`, 2 bits per weight, two weights per byte (upper 4 bits reserved).

```
00 = 0    01 = +1    10 = -1    11 = reserved (treated as 0)
```

The GEMM core reduces to addition/subtraction:

```
y_i = Σ_j w_ij · x_j   →   y_i = Σ_{w=+1} x_j  −  Σ_{w=−1} x_j
```

No FP32/FP16 multiplication in the inner loop → maximum ALU throughput even without an FPU (runs on FPU-less ARMv7-A).

## SIMD Strategy (Honest Assessment)

| Platform | Path in `hydra_engine_step()` | Status |
|---|---|---|
| ARM64/ARMv7-A with NEON | `hydra_neon_accumulate_chunk()` — 16 lanes per chunk | **active**, covered by seeded roundtrip tests on macOS ARM64 CI |
| ARM, `dim % 16 != 0` | NEON for multiples of 16, scalar for the remainder | active |
| x86 / x86-64 | purely **scalar** | no SIMD — AVX2 on the roadmap |

Both paths are **bit-identical** (same decode and accumulation semantics). NEON overflow analysis: |w| ≤ 1, |token| ≤ 127, |state| ≤ 127 → products ≤ 16129, safely within int16; accumulation in int32.

**Performance implication:** on x86, the ternary arithmetic is currently *not* SIMD-accelerated — the speed advantage there comes exclusively from the 2-bit storage format (less memory bandwidth, more cache hits) and the absence of FP multiplication. Genuine SIMD acceleration on x86 requires the AVX2 kernel (roadmap).

## Testing Strategy

Every test in the suite is falsifiable — no tautological "same input → same output" checks:

- **Hand-computed decoder cases** — single-weight models with hand-derived expected tokens prove the packing bit-order and token derivation are correct.
- **Seeded roundtrip & variance** — 5 platform-fixed LCG seeds generate random ternary weight patterns. Per seed: all 32 outputs within vocab, and a fresh engine instance must reproduce the bit-identical sequence. Across seeds: sequences must differ (no-op detection).
- **Security regressions** — bad magic, out-of-bounds weights, and `layers × dim > weights_len` headers must be rejected at load time. The last one is a confirmed OOB-read PoC: before the fix, a crafted header could read up to ~4 GiB past the mmap.
- **Dual-path validation** — macOS CI runners are ARM64, so the seeded tests exercise the real NEON path; Ubuntu CI covers the scalar path. The platform-fixed LCG guarantees identical sequences on both.

## Coexistence Axiom

```
U(s,a) = R(s,a) · I{H(s)=1}  −  ∞ · I{H(s)<1}
```

`hydra_verify_axiom()` implements the gate: `humanity <= 0` → utility pushed to `-1e9`, return code 0 (blocked). Every agent action path must pass this gate before triggering external effects.

## Security Design

- Header validation: magic, version, dim/vocab bounds, `weights_offset + weights_len ≤ file_size`, and `layers × dim ≤ weights_len` (prevents out-of-bounds reads past the mmap).
- State update with clamping (no modulo distortion, no signed-overflow UB).
- Library logs go to stderr exclusively; stdout stays clean for machine-readable output (`--json`).
- Test coverage: garbage input, OOB headers, NULL pointers, per-seed reproducibility.
