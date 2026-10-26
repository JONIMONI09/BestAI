# External Research: BitNet b1.58 / W1.58A8 and 2-bit Ternary Packing

Web-verified 2026-10-26 (arXiv papers, microsoft/BitNet). This document
records the external facts the version-3 engine and converter build on,
the exact points where the repository follows them, and — deliberately —
the points where it does not.

## 1. BitNet b1.58 (W1.58A8) — the quantisation scheme we implement

Sources:

- arXiv 2402.17764 — "The Era of 1-bit LLMs: All Large are Ternary"
  (Microsoft, b1.58 introduction).
- arXiv 2504.12285 — "The 1.58-bit LLM" (b1.58-2B-4T technical report).
- Hugging Face `bitnet-b1.58-2B-4T` model card (weights shipped as W1.58A8).

Facts:

- **Weights (W1.58)**: `W̃ = RoundClip(W / γ, −1, +1)` with
  **γ = absmean(|W|)** of the tensor — a *ternary* (1.58-bit) matrix in
  {−1, 0, +1}. (Reported as "1.58 bits" because the uniform ternary code
  carries log₂3 ≈ 1.58 bits per weight.)
- **Activations (A8)**: per-token, `x̂ = x · 127 / max(|x|)` — a
  symmetric **int8** quantisation with values in **[−127, +127]** (128
  is never used).
- **The GEMV becomes add/sub**: because the weights are ternary, the
  `W̃ᵀ · x̂` dot product is a sum over the non-zero weights of
  `± x̂_j` — no multiplies. The reference kernels pack 4 ternary weights
  per 8 bits, unpack in SRAM, and accumulate with adds/subtracts.
- The 2B-4T model uses RoPE, SubLN, and a 4096 context window, and is
  trained *natively* in the ternary/int8 domain (the ternary constraint
  is part of the training, not a post-hoc compression).

**What the repository follows (exactly):**

- Ternary weight codes {−1, 0, +1} — the same W1.58 constraint.
- **int8 state in ±127** — the exact A8 domain; `hydra_engine_step_tf()`
  clamps every activation through `tf_clamp127()` and the mat-vec is
  integer add/sub over the ternary codes (no FP multiply in the core).
- **Per-layer absmean scale** `scale = round(γ·256)` stored as u16 fixed
  point; the ternary matrix is the source divided by γ and round-clipped
  (`|w| ≥ mean/2` threshold form — identical to `round(w/γ)`), and the
  engine divides the mat-vec result by `scale/256` before the
  non-linearity. This is γ exactly, in the fixed-point form the format
  can store.

**What the repository does NOT do (stated, not hidden):**

- dim ≤ 64, vocab ≤ 1024, layers ≤ 4096 — the engine is a miniature,
  not a 2B model; the converters slice `dim × dim` corners of real
  tensors (see `docs/FORMAT.md` "Version 3").
- The FFN activation is **SwiGLU**, not the squared-ReLU of the 2B-4T
  recipe; both are standard ternary-compatible choices, SwiGLU being
  the one the repository's M4 primitive set already implements and
  tests.
- No native training: the ternary matrices come from round-clipping
  float weights at conversion time, which is exactly what the
  inference-side b1.58 scheme describes.

## 2. 2-bit packing — bitnet.cpp (I2_S) and the repository's table

Sources:

- arXiv 2410.16144 — "bitnet.cpp: A Ternary Inference Library for LLMs"
  (Table 1: 2-bit code tables).
- github.com/microsoft/BitNet (code: `I2_S` unpack; issue #412 on the
  128-element block-interleaved layout).

Facts:

- The b1.58 reference kernel packs **4 ternary weights per byte**
  (2 bits each). bitnet.cpp's `I2_S` format uses the code table
  **`00` = −1, `01` = 0, `10` = +1, `11` = unused**, in a
  128-element block-interleaved layout for SIMD alignment.
- The repository's v2/v3 files use 4 ternary weights per byte as well,
  but with the code table **`00` = 0, `01` = +1, `10` = −1, `11` = 0
  (treated as 0)** and *sequential* row-major packing.

**Decision (2026-10-26): the repository keeps its own code table.**
Any three distinct 2-bit patterns are a valid ternary code — the format
only requires that encoder and decoder agree on the same permutation.
`11` is defined as 0 (rather than being "unused") so that a stray set
bit can never crash or misdecode a model. Changing the table now would
break every v2 file already shipped and tested (and the v3 tests), for
no functional gain; the layout-identity claim is explicitly **not**
made anywhere in the documentation — the formats share a density and a
semantic, not a byte layout.

## 3. Where the numbers in the engine come from

| Constant | Value | Source |
|---|---|---|
| int8 domain | ±127 | b1.58 A8 (per-token absmax int8, 128 unused) |
| weight codes | {−1, 0, +1} | b1.58 W1.58 |
| per-layer scale | absmean × 256, u16, min 1 | b1.58 γ, fixed-point; 0 forbidden (division) |
| RoPE base | 10000 | standard LLaMA-style default (the 2B-4T uses RoPE) |
| SwiGLU | `SiLU(g)·u` | standard LLaMA-style FFN (M4 primitive) |
| quantisation gain back into int8 | 32 | engine constant (`HYDRA_TF_QUANT_GAIN`): RMSNorm output has unit RMS, so ×32 lands well inside ±127 before the clamp |
| greedy decode | `argmax_i (W_out·state)[i] mod vocab` | full-dimension; replaces the v1 column-0 read |

## 4. How this was verified

- arXiv 2410.16144 Table 1 was read directly (not from a summary) and
  found to differ from the repository's table — resolved as documented
  above, with the repository table kept.
- The engine's scale-offset bug (scale read from the w1 plane start
  instead of after it) was caught by a hand-built 2-bit model with a
  hand-derived state, where the POS case passed by SiLU-saturation
  accident and the NEG case collapsed — see `docs/session.md`
  (the engineering log) for the 2026-10-26 entry.
