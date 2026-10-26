# The `.hydra` Binary Format (v1)

> **Two different "v2"s exist in this repository and they are not the same
> thing.** This document's *version 2* is a header variant of this same
> 24-byte format (same magic `HYDRA`, `version` field = 2, 4 weights per
> byte). The `.hydra2` **container** in `docs/FORMAT-V2.md` (magic
> `HYDRA2\0\0`, 128-byte header, sparse-MoE) is a different, larger format
> that has no loader in this repository. See the disambiguation at the end of
> the Version 2 section below.

## Layout

```
Offset  Size  Field           Type
──────  ────  ──────────────  ─────────────────
0       4     magic           uint32  = 0x48594452 ("HYDR", little-endian)
4       2     version         uint16  = 1 (or 2, see "Version 2"; or 3, see "Version 3")
6       2     vocab_size      uint16  (1..1024)
8       4     dim             uint32  (1..64; versions 2 and 3 require dim % 4 == 0)
12      4     layers          uint32
16      4     weights_offset  uint32  (byte offset of the weights in the file)
20      4     weights_len     uint32  (byte length of the weights)
24      ...   weights         uint8[] (2 ternary weights per byte, v1)
```

The header is **exactly 24 bytes** (`#pragma pack(1)`, `struct pack "<IHHIIII"` in Python).

The loader decodes these fields with **explicit little-endian readers** (`rd_u16le` / `rd_u32le`) rather than `memcpy`-ing the struct into native integers, so files load identically on big-endian hosts.

## Weight Packing

Each byte carries **two** ternary weights in its lower 4 bits:

```
Bit:    7 6 5 4 3 2 1 0
        ^^^^^^^^^ ^^^^
        reserved   w2 (bits 2-3)
                  w1 (bits 0-1)
```

Precisely: **w1 = bits 0–1** (the lower pair), **w2 = bits 2–3**. Bits 4–7 are reserved and must be zero (the loader ignores them).

| Code (2 bits) | Value |
|---|---|
| `00` | 0 |
| `01` | +1 |
| `10` | −1 |
| `11` | reserved (loader treats it as 0) |

Iteration order is **row-major over layers × dim**: first all `dim` pairs of layer 0, then layer 1, and so on. Within one layer pass, each byte is consumed as w1 first, then w2.

## Version 2 (4 weights per byte)

The loader also accepts `version == 2` in this same 24-byte header. It holds
the **same model** — same `A[i]/B[i]` aggregation, same step, same everything —
packed tighter: each byte carries **four** ternary weights (2 bits each, the
same code table as v1: `00`=0, `01`=+1, `10`=−1, `11`=0) instead of two, so a
model is half as large on disk. The layout is documented here because the
loader implements it, and an implemented path that no document admits is
exactly the claim/reality drift this repository's lint exists to catch.

Per layer, the payload is two consecutive planes:

```
[ dim/4 bytes: plane w1, weight j at bits 2j..2j+1 of byte j/4 ]
[ dim/4 bytes: plane w2, same packing ]
```

Constraints and bounds (all enforced in `hydra_engine_load`, all tested in
`tests/test_engine.c`, `test_version_two_format`):

1. `dim % 4 == 0` — required, because the aggregation reads `dim/4` bytes per
   plane; without the guard the leftover dimensions would silently aggregate
   to zero. Refused with error `-8`, not rounded (.agents/rules.md R11, R14).
2. `weights_len >= layers * dim / 2` — the v2 read size. The loader validates
   the ACTUAL read size per version: a v1 file keeps its exact
   `layers * dim` requirement, a v2 file is checked against `layers * dim / 2`
   (a minimal v2 file declaring exactly the bytes it uses is therefore valid).
3. The v2 file written by `test_version_two_format` is byte-for-byte
   equivalent to the v1 file it is compared against: identical aggregates and
   identical 40-token streams. That parity test is the proof the packing is
   the same model, not just a file the loader likes.

`tools/make_model.py` and `tools/gguf_to_hydra.py` emit version 1, because
the converter's exactness guarantee (A/B written as literal layer counts) is
expressed most directly in v1's one-byte-per-pair layout. **`tools/safetensors_to_hydra.py`
is the shipped writer for version 2**: it folds the ternary column sums to
the 64-wide state and writes them as layer sums of ±1 into the two
`dim/4`-byte planes per layer, so the engine's aggregation reproduces them
value for value. The test suite also produces its own version-2 fixtures
(`test_version_two_format` in `tests/test_engine.c`, `run_safetensors` in
`tools/gguf_test.py`).

## Version 3 (per-layer non-linear blocks)

The loader also accepts `version == 3` in this same 24-byte header. Unlike
v1/v2 it does **not** describe an aggregated recurrence: it stores a full
per-layer non-linear ternary forward pass (BitNet b1.58 style: ternary
weights, int8 activations, cross-dimension BitLinear projections), which
`hydra_engine_step_tf()` executes layer by layer from the mapped bytes at
token time. The load-time `agg_a/agg_b` shortcut is **not** built for v3
models (the aggregation would no longer be an equivalent, because
non-linearities are applied between layers — see
`docs/ARCHITECTURE.md`, "Version 3 forward pass").

### Weight body layout

Same 2-bit packing and code table as v2 (`00`=0, `01`=+1, `10`=−1,
`11`=0; weight `j` of a 4-wide group lives in bits `2j..2j+1`). Per layer
`l`, four consecutive fields:

```
[ dim*dim/4 bytes: Wg block  - gate projection, row i = bytes i*(dim/4) ]
[ dim*dim/4 bytes: Wu block  - up projection,   same row layout        ]
[ dim/4 bytes:     w1 plane  - token-injection scalars, one per dim    ]
[ 2 bytes:         scale     - uint16 LE, per-layer absmean scale * 256]
```

after all `layers` layers, one final block:

```
[ dim*dim/4 bytes: W_out block - logit-mixing projection ]
```

`layer_bytes = dim*dim/2 + dim/4 + 2`. Required weight body size:
`layers * layer_bytes + dim*dim/4`.

### Meaning of the fields

- `Wg_l[i][j]`, `Wu_l[i][j]` in {−1, 0, +1}: the two SwiGLU projections of
  layer `l`. Per token and per layer the engine computes, with the int8
  state `x` (the W1.58A8 activation domain, ±127) and the integer-exact
  ternary mat-vec:
  `gate[i] = Σ_j Wg_l[i][j]·x[j]`, `up[i] = Σ_j Wu_l[i][j]·x[j]`.
- `w1_l[i]` in {−1, 0, +1}: the token injection into the RESIDUAL:
  `x_eff[i] = clamp127(state[i] + w1_l[i]·token_val)` BEFORE both
  mat-vecs (transformer embedding pattern — the same ternary token
  drive the v1 recurrence has, so seed/prefill semantics are
  preserved). The clamp keeps the activation in the int8 W1.58A8
  domain. Gate-only injection is deliberately NOT used: with `up =
  Wu·0 = 0` for a zero state, `SwiGLU(gate, 0) = 0` and the engine
  could never leave the zero state on its first token.
- `scale` (u16, = absmean(source block)·256 rounded, minimum 1): the
  per-layer absmean scale of the source tensor the converter ternarised
  (b1.58: the ternary matrix is the source divided by its absmean and
  round-clipped). The engine rescales the mat-vec results with it
  (`gate_f = gate / (scale/256)`) before the SwiGLU non-linearity, so a
  layer with a different source magnitude contributes at its real
  relative size instead of every layer looking identical.
- `W_out[i][j]` in {−1, 0, +1}: the final cross-dimension mix. After the
  last layer, `y[i] = Σ_j W_out[i][j]·state[j]` — the full-dimension logit
  vector. Decoding (greedy) takes `argmax_i y[i]` and maps it to a token
  by `token = i mod vocab`. `hydra_sample_token()` (v3 mode) runs the same
  y vector through the existing repetition-penalty / softmax / min-p /
  seeded-CDF machinery, so **every** state dimension can influence the
  output token (the v1 column-0 bottleneck does not exist in v3).

### Forward pass (executed per token, zero heap, stack buffers only)

For `l = 0 .. layers−1`:

```
xefx[i] = clamp127(state[i] + w1_l[i]·token_val)      (int8 domain)
gate[i] = Σ_j Wg_l[i][j]·xefx[j]                      (int32, exact)
up[i]   = Σ_j Wu_l[i][j]·xefx[j]                      (int32, exact)
g_f[i]  = gate[i] / (scale_l/256)        (float)
u_f[i]  = up[i]   / (scale_l/256)        (float)
h       = SwiGLU(g_f, u_f)                           (hydra_swiglu)
n       = RMSNorm(h, w=1, eps=1e-5)                  (hydra_rmsnorm)
n       = RoPE(n, pos=step_count, theta=10000)       (hydra_rope_inplace)
state[i] = clamp(round(n[i] * 32), ±127)   (back into the int8 domain)
```

The KV cache is applied **once per token, after the layer loop** (the
attention-sink pattern, not per-layer attention): if `kv != NULL` and the
cache already holds entries, `state[i] = clamp127(state[i] +
lround(sink_value[0][i]))` (the value of permanent sink slot 0), then
`hydra_kv_cache_append(kv, k = v = state)` — one entry per token,
`-17` on append failure.

Then `y = W_out · state` (int32, unscaled) and the decode above
(`token = argmax_i y[i] mod vocab`), and `step_count++` (the RoPE
position of the next token). The per-layer
non-linearities (SwiGLU → RMSNorm → RoPE → int8 quantisation) are applied
**between** layers, so the layer stack is NOT algebraically collapsible
into load-time sums — `tests/test_engine.c`
(`test_tf_multilayer_not_reducible_by_loadtime_sums`) proves a 4-layer v3
model produces states/tokens that the summed-plane single pass cannot
reproduce.

### Constraints (all enforced in `hydra_engine_load`, tested)

1. `dim % 4 == 0` (same guard as v2; error `-8`).
2. `weights_len >= layers * layer_bytes + dim*dim/4` (error `-10`), plus
   the generic `weights_offset + weights_len <= file_size` check.
3. `scale == 0` in any layer is refused at load (`-15`) — a zero scale
   would divide by zero in the forward pass.
4. v3 models use `hydra_engine_step_tf()`; the legacy `hydra_engine_step()`
   / `hydra_engine_step_fast()` refuse v3 engines with error `-14` (no
   silent fallback to the aggregated path — that would be a different
   model pretending to be this one).

### What v3 is and is not

v3 gives the engine real non-linear depth and cross-dimension mixing at
the format's existing ceiling (dim ≤ 64, vocab ≤ 1024, layers ≤ 4096): a
v3 model no longer compresses to 128 integers (its `layers * dim*dim/2`
ternary weight bits are all individually reachable at inference). It is
still not a 7B LLM: the state is 64 wide, the vocab is capped, and the
converter slices real tensors into dim×dim blocks. That is stated in the
converter report, not hidden.

## Reference Writer

`tools/make_model.py` is the canonical Python reference for version 1:

```python
header = struct.pack("<IHHIIII", MAGIC, VERSION, VOCAB, DIM, LAYERS,
                     weights_offset, num_packed_bytes)
```

Version 3 files are written by `tools/gguf_to_hydra.py --v3` and
`tools/safetensors_to_hydra.py --v3` (see "Version 3" above for the body
layout both implement). Each writer runs a **write-time roundtrip check**
in stdlib-only Python: after writing, it re-reads the file, re-decodes the
first layer's scale and the full `W_out` block, and aborts on any
mismatch, so a layout drift between writer and format fails at conversion
time, not at engine load. The conversion report states per layer which
tensor (and how much of it) each block comes from — the converter slices
the top-left `dim × dim` corner of real tensors and zero-pads when the
tensor is smaller; it does not pretend to be the full source model.

## Validation Rules (Loader, `src/hydra_engine.c`)

1. `magic == 0x48594452`
2. `version == 1`, `2` or `3` (versions 2 **and** 3 additionally require `dim % 4 == 0`, see their sections)
3. `1 <= dim <= 64` and `1 <= vocab_size <= 1024`
4. `layers <= 4096` (`HYDRA_MAX_LAYERS`) — bounds the worst-case accumulator magnitude to `4096 × 254`, well below `INT32_MAX`. (Discovered via proof-of-concept: without this rule, `layers = 16909321` with `dim = 1` and all weights `+1` overflows a signed accumulator after ~16.9 MB of file.)
5. `weights_offset >= 24` — the weight region may not overlap the header, otherwise header bytes would be executed as weights.
6. `weights_offset + weights_len <= file_size` — prevents out-of-bounds reads past the mmap. **The sum is computed in 64 bits**, even though both operands are `uint32`: in 32-bit arithmetic `0xFFFFF000 + 0x1000` wraps to `0`, the model looks valid, and the aggregation walk runs off the mapping. Measured: with a 32-bit sum, `tests/test_large_model.c` loses its child process with SIGSEGV.
7. The required weight body must be covered by `weights_len`, sized per version: `layers * dim` (v1), `layers * dim / 2` (v2), `layers * (dim*dim/2 + dim/4 + 2) + dim*dim/4` (v3). The loader reads exactly those bytes — the v1/v2 aggregates once at load, v3 directly from the mapping at token time — so the declared weight region must cover them. (Discovered via proof-of-concept: without this rule, a crafted header could read up to ~4 GiB past the file mapping.)
8. v3 only: no layer may carry `scale == 0` (a zero scale would divide by zero in the forward pass); the loader walks the mapped scale words once at load and refuses the file.

If any load rule is violated, the mapping is torn down immediately through a single cleanup path and a negative error code is returned (`-5` magic, `-7` version, `-8` dim/vocab, `-9` file bounds, `-10` weight region, `-11` layer cap, `-12` header overlap, `-15` v3 zero scale). Runtime guards on top: `hydra_engine_step()`/`hydra_engine_step_fast()` refuse v3 engines with `-14` (and `hydra_engine_step_tf()` refuses v1/v2 engines with the same code), the TF forward pass returns `-16` on any non-finite intermediate, and a failed KV append returns `-17` (see the "Version 3" section above).

## Model Size vs RAM (and what "32-bit" actually limits)

**The whole model is always loaded**, whatever the RAM looks like. `hydra_engine_load()`
maps the file read-only with a single `mmap` and never copies weights into the heap; the
kernel faults pages in as the inference touches them. A model therefore needs RAM
proportional to the pages it *actually reads*, not to its size on disk.

Measured on this host (`tests/test_large_model.c`, `make test-run`):

| Case | Result |
|---|---|
| 5 GiB sparse `.hydra`, 24-byte header + real weights, hole behind them | loads, generates, `mapped_size == 5 GiB` |
| Same, on a container with 7 GiB free disk | 7 checks, 0 failures — sparse costs a few KiB |

The sparse trick is what makes the test cheap *and* honest: it is the same 5 GiB file the
loader sees, not a smaller stand-in.

What genuinely limits 32-bit (`armeabi-v7a`), in the order you hit it:

1. **Address space, not RAM.** A 32-bit process has ~3 GiB of user virtual address space
   (1 GiB reserved for the kernel on armv7, more with a 3G/3G split). A mapping larger than
   that cannot exist: `mmap` fails and the loader returns `-4`. This is a platform limit, not
   a bug, and no amount of laziness in the engine avoids it.
2. **32-bit file offsets.** `weights_offset` and `weights_len` are `uint32` in v1, so weights
   must start below 4 GiB. A header that claims otherwise is *refused* (rule 6), not
   truncated — the safe behaviour.
3. **Nothing else.** There is no header-level size limit: `dim`, `layers` and the weight
   length are independent of the file size, and the loader never allocates proportionally to
   the file.

Supporting a single model whose weights start at or above 4 GiB needs the
`.hydra2` container (64-bit offsets, `docs/FORMAT-V2.md` — not yet implemented)
or a split mapping with `mmap64`/`off64_t`. Header version 2 (4 weights per
byte) is *not* the answer to that question: it halves the file but keeps the
32-bit offsets. It does **not** need more RAM, which is the point: the
requirement "complete loading regardless of RAM" is already met on 64-bit
platforms.

## Density, stated exactly (Honest Note)

The format stores **2 ternary weights per byte, i.e. 4 bits per weight on disk**: `w1` in bits 0–1, `w2` in bits 2–3, with the upper 4 bits reserved. One byte therefore covers one `(layer, dim)` pair, and `weights_len = layers × dim` **bytes** (verified: a 4-layer, dim-64 model declares `weights_len = 256`).

The reserved code `11` is decoded as `0`, so a byte carries `3 × 3 = 9` distinct states, i.e. `log₂9 ≈ 3.17` bits of information **per byte** — about **1.58 bits per weight** of information inside 4 allocated bits. The honest figures are therefore:

- **allocated (v1):** 4 bits per weight (two 2-bit codes in 8 bits),
- **informational:** ~1.58 bits per weight,
- **allocated (v2, header version 2):** 2 bits per weight, no reserved bits.

Packing 4 weights per byte (header version 2) halves the file for the same model: v1's 4 bits per weight (half of each byte reserved) become exactly the 2-bit code of every weight.

## Planned Extensions (v2)

> **The v2 container is NOT the aggregated `A[i]/B[i]` form.** The canonical v2
> specification is [`docs/FORMAT-V2.md`](FORMAT-V2.md), and it specifies a
> sparse-MoE layout. The aggregated-container idea below is a **superseded
> exploratory sketch**; see *Roadmap status* in `FORMAT-V2.md` for why it lost,
> and for the note that it remains available as a v1.1 file-level optimisation.

- 64-bit offset fields, so weights may start at or above 4 GiB (see "Model Size vs RAM" —
  this is the only size limit that RAM does not solve)
- Per-layer scale factors (γ from absmean quantization)
- Checksum (xxHash) over the weight region
- 4-weights-per-byte packing — **done, shipped as header version 2** (see the
  "Version 2" section above); it was the first item of this list and is no
  longer a plan
- **Aggregated container** holding only `A[i] = Σ_l w1[l][i]` and `B[i] = Σ_l w2[l][i]`, signalled by a `HYDRA_AGGREGATE_V2` flag. The layer loop in v1 is algebraically redundant (see "Known Structural Redundancy" in `docs/ARCHITECTURE.md`), so v1 models do compress losslessly into this form, and v1 files would keep loading unchanged. **Superseded as the v2 design** — see the note above.
