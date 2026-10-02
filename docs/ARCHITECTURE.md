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
| `src/hydra_neon.h` | NEON SIMD kernel — **actively integrated**: `step()` uses it on ARM in 16-lane chunks; bit-identical scalar fallback for remainders and x86 (proven by a same-build comparison test) |
| `tools/make_dummy_model.py` | Test model generator (reference implementation of the format) |
| `tests/test_engine.c` | 49 unit tests (hand-computed cases, NEON-vs-scalar, seeded roundtrips, security regressions) |
| `server.js` | Web console server (Node, zero npm dependencies) |
| `public/` | Hydra-Stone Console frontend (plain HTML/CSS/JS) |

## Memory Model: Zero-Heap Inference via mmap

1. Weights are **never** copied into the heap; the inference path performs no `malloc` at all.
2. `mmap(PROT_READ, MAP_SHARED)` maps the file into the address space.
3. `madvise(MADV_SEQUENTIAL)` tells the kernel the access pattern → aggressive read-ahead. This is a *sequencing* hint, **not** an eviction hint.
4. 4-KiB pages are faulted in on demand and are clean → reclaimable under memory pressure without swap traffic.
5. The runtime state is a **fixed** `int8_t[64]` vector plus a `int64_t[64]` stack accumulator → O(1) process memory, independent of model size.

**Honest statement of what is (and is not) guaranteed.** The engine's *own* memory usage is constant and independent of model size: a fixed 512-byte state vector and a 512-byte accumulator, full stop. The weight pages are owned by the **kernel page cache**, not by the engine. Because `step()` walks the whole weight region per token, the working set is the entire file: each token re-faults the pages that were evicted. So:

- **Guaranteed:** zero heap allocation, O(1) engine RSS, no swap traffic (the mapping is `PROT_READ`/clean).
- **Not guaranteed:** a zero resident set. Claiming otherwise would be untrue — `MADV_SEQUENTIAL` only controls read-ahead behaviour.
- **Opt-in:** build with `-DHYDRA_DROP_CACHE` to issue `madvise(MADV_DONTNEED)` after every step, handing the touched pages back to the kernel. This trades throughput (a fresh page fault per 4 KiB per token) for a minimal resident set, and is the right knob on RAM-constrained devices.

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
| ARM64/ARMv7-A with NEON | `hydra_neon_accumulate_chunk()` — 16 lanes per chunk | **active**, proven bit-identical to the scalar reference by `test_neon_matches_scalar` in the same build |
| ARM, `dim % 16 != 0` | NEON for multiples of 16, scalar for the remainder | active |
| x86 / x86-64 | purely **scalar** | no SIMD — AVX2 on the roadmap |

Both paths are **bit-identical**, and that claim is now *tested* rather than asserted: on ARM the suite runs a scalar reference implementation of `step()` over the same model in the same binary and compares token sequences **and** state vectors. Empirically confirmed on three targets — host x86-64, ARM64 under NEON, and the Android emulator — which all emit the token sequence `211, 36, 185, 410, 7, 40, 197, 478` for `models/demo.hydra` with `start_token=42`.

NEON overflow analysis: `|w| ≤ 1`, `|token| ≤ 1023`, `|state| ≤ 127` → per-lane product ≤ 1150, safely within int16; accumulation happens in an `int64_t` array, which cannot overflow for any layer count the loader accepts.

**Performance implication:** on x86, the ternary arithmetic is currently *not* SIMD-accelerated — the speed advantage there comes exclusively from the compact storage format (less memory bandwidth, more cache hits) and the absence of FP multiplication. Genuine SIMD acceleration on x86 requires the AVX2 kernel (roadmap).

## Known Structural Redundancy: the Layer Loop Is Algebraically Free

Inside the layer loop, both `token_val` and `state_vector[i]` are **loop-invariant**. The accumulation therefore collapses:

```
acc[i] = Σ_l ( w1[l][i]·token + w2[l][i]·state[i] )
       = A[i]·token + B[i]·state[i]      with A[i] = Σ_l w1[l][i],  B[i] = Σ_l w2[l][i]
```

Consequences, stated plainly:

- **Inference is equivalent to a single recurrence** with `2·dim` aggregates in `[−layers, +layers]`. Depth buys no expressive power in v1.
- **Every v1 model is losslessly compressible** by a factor of roughly `4·layers / ⌈log₂(2·layers+1)⌉` (≈ 4× at `layers = 4`) by pre-summing to `A[i]/B[i]`.
- **This is not a bug and the v1 format is not being changed.** Retaining the layer dimension keeps the door open for genuine depth-dependent behaviour (per-layer scales, ordering, future residual structure) and preserves backwards compatibility.
- **Proposed design:** an aggregated v2 container carrying only `A[i]`/`B[i]`, signalled by a new `HYDRA_AGGREGATE_V2` capability flag in the header plus a distinct loader path and error code, so v1 files keep loading unchanged. Spec lives here as a design proposal; the implementation is roadmap work.

## Testing Strategy

Every test in the suite is falsifiable — no tautological "same input → same output" checks:

- **Hand-computed decoder cases** — single-weight models with hand-derived expected tokens prove the packing bit-order and token derivation are correct. `w=0x02, token=5` now expects `1`, not `11`: the old value was the *symptom* of `labs()` destroying the sign, not the intended behaviour.
- **NEON vs. scalar in the same build** — a scalar reference `step()` runs alongside the SIMD kernel on ARM and must agree bit-for-bit on tokens and state. This test exists because it was missing when the NEON sign inversion shipped.
- **Seeded roundtrip & variance** — 5 platform-fixed LCG seeds generate random ternary weight patterns. Per seed: all 32 outputs within vocab, and a fresh engine instance must reproduce the bit-identical sequence. Across seeds: sequences must differ (no-op detection).
- **Security regressions** — bad magic, out-of-bounds weights, `layers × dim > weights_len`, `weights_offset` inside the header, and the `layers = 16 909 321` signed-overflow PoC must all be rejected at load time. Every case was a reproduced bug before it became a test.
- **Axiom edge cases** — `NaN` and `±Inf` must block; `NaN <= 0.0f` is `false` in IEEE-754, so without an explicit `isfinite` check a NaN would bypass the safety gate entirely.
- **Endianness** — the loader decodes the header with explicit little-endian readers; tests write with explicit little-endian writers, so the suite is meaningful on big-endian hosts too.
- **Temp files** — `mkstemp()` with cleanup, so parallel runs cannot collide and a hostile `/tmp` cannot be pre-seeded with a symlink.
- **Dual-path validation** — macOS CI runners are ARM64, so the NEON comparison test runs there; Ubuntu CI covers the scalar path.

## Coexistence Axiom

```
U(s,a) = R(s,a) · I{H(s)=1}  −  ∞ · I{H(s)<1}
```

`hydra_verify_axiom()` implements the gate: a **non-finite** `humanity_factor`, or one `<= 0`, pushes the utility to `-1e9` and returns `0` (blocked); a non-finite `proposed_score` is rejected the same way. The finiteness check is not defensive decoration — `NaN <= 0.0f` evaluates to `false` in IEEE-754, so without it a NaN would sail through the gate and return `1` (allowed). Every agent action path must pass this gate before triggering external effects.

## Security Design

- **Header validation**, in order: magic → version → `dim`/`vocab` bounds → `layers ≤ HYDRA_MAX_LAYERS` (4096) → `weights_offset ≥ sizeof(header)` → `weights_offset + weights_len ≤ file_size` → `layers × dim ≤ weights_len`.
- **Overflow-free accumulation** — the layer cap bounds the worst case to `4096 × 254 = 1 040 384`, far below `INT32_MAX`; the accumulator is `int64_t` as defence in depth.
- **Single cleanup path** — every loader failure routes through one `goto fail` that clears `fd` and the mapping, so a subsequent `hydra_engine_unload()` cannot double-close a descriptor.
- **Signed token derivation** — `acc mod vocab`, lifted into `[0, vocab)`, so `w = +1` and `w = −1` remain distinguishable. `labs()` erased that distinction.
- **Full vocabulary reachability** — the token contributes its full `uint16_t` value; no `& 0x7F` truncation that would collapse every eighth token ID above a 128-entry vocabulary.
- Library logs go to stderr exclusively; stdout stays clean for machine-readable output (`--json`).
- Test coverage: garbage input, OOB headers, header-overlap offsets, the layer-overflow PoC, NULL pointers, NaN/Inf axioms, per-seed reproducibility, NEON-vs-scalar equality.
