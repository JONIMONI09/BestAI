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
| `include/hydra_model.h` | Public API, header struct, defines; also the sampler config (`hydra_sampler_config_t`), the transformer primitives (`hydra_rmsnorm`/`hydra_rope_inplace`/`hydra_swiglu`) and the streaming `hydra_kv_cache_t` |
| `src/hydra_engine.c` | Loader (mmap), inference step, axiom gate, the min-p/repetition-penalty sampler, and the zero-heap transformer primitives + KV cache |
| `src/main.c` | CLI entry point (`--json` mode, `--temp`/`--min-p`/`--rep-penalty`/`--rep-window`/`--seed`) |
| `src/hydra_neon.h` | NEON SIMD kernel — **actively integrated**: `step()` uses it on ARM in 16-lane chunks; bit-identical scalar fallback for remainders and x86 (proven by a same-build comparison test) |
| `src/hydra_simd.h` | AVX2 kernel — **actively integrated** on x86-64 `-mavx2` builds: `hydra_avx2_accumulate_agg` (step) + `hydra_avx2_unpack_v2_plane` (load-time v2 LUT), runtime CPU dispatch with scalar fallback (proven by `test_avx2_step_kernel` / `test_avx2_v2_unpack`) |
| `tools/make_model.py` | Test model generator (reference implementation of the format) |
| `tools/gguf_to_hydra.py` | GGUF → `.hydra` (v1) converter, absmean ternarisation |
| `tools/safetensors_to_hydra.py` | safetensors → `.hydra` (version 2, 4 ternary weights/byte) converter, stdlib only |
| `tests/test_engine.c` | unit tests (hand-computed cases, NEON-vs-scalar, AVX2-vs-scalar, sampler suite, transformer primitives + KV cache, security regressions). The count is architecture-dependent and drift-gated: see `tools/test_count_baseline.txt` |
| `server.js` | Web console server (Node, zero npm dependencies) |
| `public/` | Hydra-Stone Console frontend (plain HTML/CSS/JS) |

## Memory Model: Zero-Heap Inference via mmap

1. Weights are **never** copied into the heap; the inference path performs no `malloc` at all.
2. `mmap(PROT_READ, MAP_SHARED)` maps the file into the address space.
3. `madvise(MADV_SEQUENTIAL)` tells the kernel the access pattern → aggressive read-ahead. This is a *sequencing* hint, **not** an eviction hint.
4. 4-KiB pages are faulted in on demand and are clean → reclaimable under memory pressure without swap traffic.
5. The runtime state is **fixed-size and independent of model size**: the recurrent state `int8_t[64]` (64 B), the two load-time aggregates `int32_t[64]` each (512 B for `agg_a` + `agg_b`), and an `int64_t[64]` accumulator (512 B, stack-local inside `hydra_engine_step`). Nothing in that list grows with `layers` or with the file size.

**`sizeof(HydraEngine)` = 672 bytes on 64-bit (LP64) platforms**
(measured with `sizeof` on x86-64; pinned at compile time on all LP64
targets by `hydra_engine_size_check` in `include/hydra_model.h`, which
CI compiles on macOS arm64 and the AArch64 NEON leg. The pin is
LP64-conditional: 32-bit ABIs such as armeabi-v7a use a smaller ILP32
layout, and an unconditional pin broke the NDK 32-bit compile —
caught by CI's android-lint NDK step on 2026-10-26):

| field | bytes |
|---|---|
| `int fd` + 4 B alignment padding | 8 |
| `const uint8_t *mapped_weights` | 8 |
| `size_t mapped_size` | 8 |
| `HydraModelHeader header` (packed) | 24 |
| `int8_t state_vector[64]` | 64 |
| `int32_t agg_a[64]` | 256 |
| `int32_t agg_b[64]` | 256 |
| `double load_ms` | 8 |
| `size_t weights_bytes` | 8 |
| `long resident_kb_after_load` | 8 |
| `long rss_kb_before_load` | 8 |
| `uint64_t rng_state` (sampler) | 8 |
| `uint32_t step_count` (v3 RoPE position) + 4 B trailing padding | 8 |
| **total** | **672** |

(The 656 B that the README claimed until 2026-10-26 was the size before
`rng_state` was added for min-p sampling; 664 B was the size before
`step_count` was added for the version-3 RoPE position.
`include/hydra_model.h` now pins 672 at compile time on every LP64
platform, so the number can never silently drift again.)

The **v1/v2** per-token step touches only 576 B of that (the two aggregate
arrays plus the state vector); the rest is load-time bookkeeping. The v3
path (`hydra_engine_step_tf`) does not use the aggregates at all — it
reads the weight planes directly from the mapping and keeps all of its
working set in stack-local arrays (see "Version 3 forward pass" below).

**Honest statement of what is (and is not) guaranteed.** The engine's *own*
memory is ~1.1 KiB of fixed buffers — the state vector, both aggregate vectors
and the accumulator — and is independent of model size. The weight pages are
owned by the **kernel page cache**, not by the engine.

The weight region is **not** re-read per token. `hydra_build_aggregation()`
walks it exactly **once**, at load, and folds it into `A[i]`/`B[i]`; after that
the recurrent step executes `2 * dim` multiply-accumulates per token (128 at
`d = 64`), invariant to layer depth. So the per-token working set is the fixed
buffers, not the file. So:

- **Guaranteed:** no heap allocation for weights, fixed-size engine buffers, no swap traffic (the mapping is `PROT_READ`/clean).
- **Not guaranteed:** a zero resident set. The mapped weight pages are real
  physical memory until the kernel reclaims them; a zero resident set would be untrue
  (`MADV_SEQUENTIAL` only controls read-ahead, and an explicit
  `MADV_DONTNEED` was tried, measured and rejected — it made RSS worse).
- **Opt-in:** build with `-DHYDRA_DROP_CACHE` to issue `madvise(MADV_DONTNEED)`
  after every step, handing the touched pages back to the kernel. This trades
  throughput for a minimal resident set, and is the right knob on
  RAM-constrained devices.

## Ternary Math

Weights: `W ∈ {-1, 0, +1}`, two weights per byte (v1: `w1` in bits 0–1, `w2` in bits 2–3; upper 4 bits reserved → 4 bits per weight; header version 2 packs 4 weights per byte → 2 bits per weight). One byte covers one `(layer, dim)` pair, so `weights_len = layers × dim` bytes (v1).

```
00 = 0    01 = +1    10 = -1    11 = reserved (treated as 0)
```

The GEMM core reduces to addition/subtraction:

```
y_i = Σ_j w_ij · x_j   →   y_i = Σ_{w=+1} x_j  −  Σ_{w=−1} x_j
```

No FP32/FP16 multiplication in the inner loop → maximum ALU throughput even without an FPU (runs on FPU-less ARMv7-A).

## Transformer Primitives and Streaming KV Cache (M4)

The recurrent core above is not a transformer. To make a real transformer
head viable over the same engine, four zero-heap, R14-guarded building
blocks were added (`include/hydra_model.h` API, `src/hydra_engine.c`
implementation). Every one validates its arguments **and** checks all input
finiteness before any output store, so an error return never leaves a
partially written result — the same safety posture as the loader.

- `hydra_rmsnorm(x, w, dim, eps, out)` — `y = x/√(mean(x²)+ε)·w`, in-place
  safe, `dim` 1..4096.
- `hydra_rope_inplace(x, dim, pos, theta_base)` — standard rotary embedding
  on `(x[2i], x[2i+1])` pairs with `freq_i = θ^(−2i/dim)`.
- `hydra_swiglu(g, u, dim, out)` — `SiLU(g)·u` with an overflow-safe sigmoid
  (branch on the sign of `x` so `exp` never overflows on either tail).
- `hydra_kv_cache_t` + `hydra_kv_cache_init/append/size/get/sink` — a
  streaming attention cache: `num_sinks` **permanent sink slots**
  (streaming-LLM attention sinks, never evicted) plus a circular window
  over `capacity − num_sinks` slots that overwrites its oldest entry.
  Caller-provided storage (zero heap), O(1) memory, O(1) per append and
  lookup.

These are float building blocks, and they are now wired into a forward
pass over the ternary core: the **version-3** model format runs
SwiGLU → RMSNorm → RoPE between real per-layer ternary projections, and
the KV cache carries one state per token (see "Version 3 forward pass"
below). The **bridge to the engine's int8 state world** is itself
tested: the SwiGLU output is quantised through the engine's own `±127`
clamp and shown to be in-range and idempotent, so a feed-forward block's
output can feed the recurrent state without the engine rejecting or
wrapping it. The KV cache's eviction is tested to preserve sinks while
sliding the window (see `test_kv_cache`).

## Version 3 forward pass (per-layer non-linear ternary stack)

Version 1 and 2 of the `.hydra` format store an *aggregated* recurrence:
the loader pre-sums the layer weights into `agg_a[i]`/`agg_b[i]` and the
per-token step is `A[i]·token + B[i]·state[i]` (see "Known Structural
Redundancy" below for what that implies). Version 3 is a different
architecture, not a repackaging: the loader keeps the per-layer ternary
weight matrices in the mapping, and `hydra_engine_step_tf()` executes a
real forward pass through all `layers` at every token.

Per token, for `l = 0 .. layers−1` (all working memory is stack-local;
the weights are read directly from the mmap — zero heap, zero copies):

```
xeff[i]  = clamp127(state[i] + w1_l[i]·token)     token injection, int8
gate[i]  = Σ_j Wg_l[i][j]·xeff[j]                 ternary BitLinear mat-vec
up[i]    = Σ_j Wu_l[i][j]·xeff[j]                 (int32, exact: ±1 × int8)
g_f[i]   = gate[i] / (scale_l/256)                per-layer absmean rescale
u_f[i]   = up[i]   / (scale_l/256)
h        = SwiGLU(g_f, u_f)                       (hydra_swiglu)
n        = RMSNorm(h, w=1, eps=1e-5)              (hydra_rmsnorm)
n        = RoPE(n, pos=step_count, theta=10000)   (hydra_rope_inplace)
state[i] = clamp(round(n[i]·32), ±127)            back into the int8 domain
```

after the loop: the attention-sink residual from KV cache slot 0 (when a
cache is given and non-empty), one `kv_cache_append` of the final state,
then the **full-dimension** logit mix `y[i] = Σ_j W_out[i][j]·state[j]`
and a greedy decode over all `dim` entries (`token = argmax_i y[i] mod
vocab`). `hydra_sample_token()` (v3 mode) feeds the same `y` vector
through the existing repetition-penalty / softmax / min-p / seeded-CDF
sampler, so every state dimension can influence the output token.

Design notes, stated as facts:

- **The mat-vec is the BitNet b1.58 "W1.58A8" pattern**: ternary weights
  ({−1, 0, +1}) × int8 activations (±127), where the float GEMV becomes
  integer add/sub per non-zero weight. The per-layer `scale` is the
  source tensor's absmean (b1.58's γ) in fixed point (×256, u16); the
  ternary matrix is `clip(round(W/γ), ±1)` of a real tensor, so the
  rescaled mat-vec reproduces the source magnitude. See `.agents/docs/RESEARCH.md`
  for the sources (arXiv 2402.17764, 2504.12285; microsoft/BitNet).
- **Depth is real, not cosmetic.** SwiGLU, RMSNorm and RoPE run
  *between* layers, so the layer stack does not collapse into load-time
  sums: `test_tf_multilayer_not_reducible_by_loadtime_sums` builds a
  4-layer model whose state/tokens a summed-plane single pass provably
  cannot reproduce, and `test_tf_dim_positive_weight_changes_stream`
  shows a weight flip in any dimension `d > 0` changes the token stream
  (the v1 column-0 bottleneck does not exist in v3).
- **The legacy path stays legacy.** `agg_a`/`agg_b` are only built and
  used for v1/v2; `hydra_engine_step()` and `hydra_engine_step_fast()`
  refuse v3 engines with `-14`, and `hydra_engine_step_tf()` refuses
  v1/v2 engines with the same code — no silent cross-version fallback.
- **The per-layer pass is scalar** (`tf_row_dot` walks 2-bit codes with
  int32 accumulation). There is no SIMD kernel for the v3 path yet; the
  SIMD table below applies to the v1/v2 step. `dim ≤ 64` keeps the cost
  to `layers·dim²` add/subs per token, exact in int32 for every layer
  count the loader accepts (`4096 · 64 · 127 ≈ 3.3e7 << INT32_MAX`).

The format details (block layout, packing, error codes, forward-pass
contract) are canonical in `docs/FORMAT.md`, "Version 3".

## SIMD Strategy (Honest Assessment)

| Platform | Path in `hydra_engine_step()` | Status |
|---|---|---|
| ARM64/ARMv7-A with NEON | `hydra_neon_accumulate_agg()` — 16 values per fixed-width chunk (4 int32 lanes per vector op) | **active**, proven bit-identical to the scalar reference by `test_neon_matches_scalar` in the same build |
| ARM, `dim % 16 != 0` | NEON for multiples of 16, scalar for the remainder | active |
| x86-64, `-mavx2` build, CPU has AVX2 | `hydra_avx2_accumulate_agg()` — 8 dims per 256-bit iteration (a `__m256i` holds 8 int32), int32 math exact under the layer cap | **active**, proven bit-identical by `test_avx2_step_kernel` in the same build |
| x86-64, `-mavx2` build, CPU lacks AVX2 | runtime `hydra_simd_has_avx2()` check → scalar fallback | active (portable by design) |
| x86, plain (non-`-mavx2`) build | purely **scalar** | no AVX2 instruction in the binary at all |

All three paths are **bit-identical**, and that claim is *tested* rather than
asserted: on ARM the suite runs a scalar reference implementation of `step()`
in the same binary and compares token sequences and state vectors, and on
x86-64 the AVX2 kernel is compared element-for-element against the scalar
reference over LCG-random matrices and the layer-cap extremes. Confirmed
empirically on three targets — host x86-64 (AVX2 where present), ARM64/NEON
under qemu, and the x86_64 Android emulator — which all emit
`[471, 386, 385, 386, 385, 386, 385, 386]` for `models/starter.hydra` with
`start_token=42` and 8 steps. The baseline is re-verified on the AVX2 build
specifically, so the SIMD path is the one that is proven end to end.

Note that the sequence is *not* a constant cycle. `B[0] != 0` in the shipped starter model, so the recurrent state genuinely contributes to `acc[0]` and the output varies. An earlier starter model had `B[0] == 0`, which made it state-blind: it looped `471, 42, 471, 42` forever and `--prompt` could have no observable effect. `tools/make_model.py` now guarantees `A[0] != 0` and `B[0] != 0` by construction and asserts it.

NEON overflow analysis: `|w| ≤ 1`, `|token| ≤ 1023`, `|state| ≤ 127` → per-lane product ≤ 1150, safely within int16; accumulation happens in an `int64_t` array, which cannot overflow for any layer count the loader accepts.

AVX2 overflow analysis: the step kernel accumulates in **int32** lanes, and that is provably exact, not a matter of luck. The loader caps `|A[i]|,|B[i]| ≤ layers ≤ 4096`, and `|token| ≤ 1023`, `|state| ≤ 127`, so the worst-case lane is `|4096·1023 + 4096·(−127)| = 4,710,400 < INT32_MAX` — no wraparound is possible, and the `cvtepi32_epi64` widening to the `int64_t` accumulator is a pure re-interpretation. The parity test is the proof, the bound is the reason it can pass.

**Performance implication:** on x86-64 with AVX2, the step is now SIMD-accelerated (8 dims per iteration for the 64-dim standard model) *and* the storage is compact (less memory bandwidth, more cache hits) and FP-multiplication-free. The v2 plane unpack is also SIMD-decoded at load time (`hydra_avx2_unpack_v2_plane`); the 4-weights/byte scatter to stride-4 dimensions stays a small scalar loop there — a 4×16 byte transpose is not cleanly expressible with plain AVX2 shuffles, and it runs once at load, not per token. AVX-512 is not implemented.

## Known Structural Redundancy: the v1 Layer Loop Is Algebraically Free

**Scope: version 1 (and version 2, which carries the same aggregated
model in a tighter packing) only.** This section describes why the
v1/v2 recurrence collapses to load-time sums — and the version-3
format exists precisely to remove that redundancy (per-layer
non-linearities between real projections, "Version 3 forward pass"
above; `test_tf_multilayer_not_reducible_by_loadtime_sums` is the
proof that the v3 stack does *not* collapse).

Inside the v1 layer loop, both `token_val` and `state_vector[i]` are **loop-invariant**. The accumulation therefore collapses:

```
acc[i] = Σ_l ( w1[l][i]·token + w2[l][i]·state[i] )
       = A[i]·token + B[i]·state[i]      with A[i] = Σ_l w1[l][i],  B[i] = Σ_l w2[l][i]
```

Consequences, stated plainly:

- **Inference is equivalent to a single recurrence** with `2·dim` aggregates in `[−layers, +layers]`. Depth buys no expressive power in v1.
- **Every v1 model is losslessly compressible** by a factor of roughly `4·layers / ⌈log₂(2·layers+1)⌉` (≈ 4× at `layers = 4`) by pre-summing to `A[i]/B[i]`.
- **This is not a bug and the v1 format is not being changed.** Retaining the layer dimension keeps the door open for genuine depth-dependent behaviour (per-layer scales, ordering, future residual structure) and preserves backwards compatibility.
- **Proposed design — SUPERSEDED, do not implement from here.** An aggregated
  v2 container carrying only `A[i]`/`B[i]`, signalled by a new
  `HYDRA_AGGREGATE_V2` capability flag in the header plus a distinct loader path
  and error code, was sketched here as a design proposal. **It is not the v2
  format.** `docs/FORMAT-V2.md` is the canonical v2 specification; the design it
  specifies is sparse-MoE, which changes what is possible rather than halving a
  file. The observation below is still correct and worth keeping — an aggregated
  container would be a lossless *file-level* optimisation — but shipping it as
  "v2" would deliver a file holding nothing the running engine does not
  already compute at load time into ~1.1 KiB of RAM. If it is ever revived it
  belongs as a v1.1 optimisation, not as a second version.

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
- **Version-3 depth proofs** — hand-built byte-precise 2-bit models with hand-derived states/tokens (the POS/NEG pair that caught the scale-offset bug: the POS case passed by saturation accident, the NEG case exposed it), a dim-`d>0` weight-flip contrast proving the token stream depends on every weight, a 4-layer non-reducibility proof against load-time plane sums, KV sink/append semantics on the TF path, prefill ≡ manual `step_tf` replay, seeded-sampling determinism across engines, and the loader's v3 validation (zero scale → −15, short body → −10, cross-version step refusal → −14). All in `tests/test_engine.c`.
- **Converter v3 gates** — `tools/gguf_test.py` converts a real (hand-built) GGUF and a real safetensors file with `--v3`, verifies the file layout byte-for-byte (header, body length, non-zero scales, non-zero W_out), re-runs the C engine twice for determinism, and checks the write-time roundtrip report; `--dim 10` and `--v3-layers 0` must be refused.

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
