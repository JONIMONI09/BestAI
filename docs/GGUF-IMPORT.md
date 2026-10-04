# GGUF Import

How a `.gguf` checkpoint becomes a `.hydra` model — and, more importantly, what
that conversion does **not** do.

Read this before assuming a converted file is "your model, but ternary".

---

## 1. The short version

```bash
# read-only: what is in this file?
python3 tools/gguf_inspect.py model.gguf

# convert
python3 tools/gguf_to_hydra.py model.gguf --out models/converted.hydra

# or through the web console
POST /api/models/import?name=model.gguf
```

Python **standard library only** — no numpy, no torch, no pip install. The
converter streams, so it does not have to hold the whole tensor in memory.

---

## 2. What the engine actually is

This is the part that matters, and the part that is easy to get wrong.

`hydra_engine_step()` computes:

```
acc[i] = A[i] * token + B[i] * state[i]
token_out = ((acc[0] mod vocab) + vocab) mod vocab + token + 1  (mod vocab)
```

`A` and `B` are **per-dimension column sums** of two weight planes, folded into
`int32` once at load time (`hydra_build_aggregation`, `src/hydra_engine.c`).
The state is a fixed `int8_t[64]` vector. There is no attention, no softmax, no
KV cache, and no hidden dimension larger than 64.

A Llama GGUF is a stack of attention and feed-forward matrices. **There is no
mapping that makes one run the other.** A "converted Llama" here is not a
smaller Llama. It is a projection of the real float weights into the
aggregated recurrent model that `.hydra` can actually express.

The converter's own report says this out loud, under `whatThisIs`:

> a projection of the real float weights into the aggregated recurrent model
> the .hydra format can express. It is not a Llama: `hydra_engine_step()`
> computes `A*token + B*state`, not attention. Train it further with
> `/api/train`.

Treat the output as a **seed model derived from your checkpoint**, not as a
reproduction of it.

---

## 3. Ternarisation: absmean (BitNet b1.58)

```
gamma   = 1 / mean(|W|)          over the rows actually read
W_tilde = clip(round(gamma * W), -1, +1)
```

`gamma` is the reciprocal mean absolute value — the scaling rule used by BitNet
b1.58 and by scalable ternary PTQ. The report carries it per plane:

| Field | Meaning |
|---|---|
| `meanAbs` | mean absolute weight over the rows read |
| `gamma` | `1 / meanAbs` |
| `ternaryZeros` | weights that rounded to `0` |
| `density` | `1 - zeros/count` — the fraction that stayed ±1 |
| `rowsRead` / `rowsTotal` | how much of the tensor was actually consumed — `rowsRead` is a **prefix** of `rowsTotal`, see §6 |
| `rescale` | final fold factor, see §5 |

**Rounding is half-away-from-zero, deliberately.** Python's built-in `round()`
is half-to-**even**, and the two disagree on every exact `.5` — which in a small
dense matrix is most of the entries. Using the built-in would make the
converter quietly disagree with itself depending on parity.

---

## 4. Which GGUF files are accepted

Four layouts are decoded — the three dense float types plus one
block-quantised type:

| `ggml_type` | Value | Bytes/element | Kind |
|---|---|---|---|
| `F32` | 0 | 4 | dense float |
| `F16` | 1 | 2 | dense float |
| `BF16` | 30 | 2 | dense float |
| `Q8_0` | 8 | 34 per 32 values | **block-quantised, dequantised at read time** |

`Q8_0` is the one block layout implemented, because it is also the one whose
format is short enough to read without guessing. Each 34-byte block is an F16
scale `d` followed by 32 signed int8 values, and dequantises as `w[i] = d *
qs[i]` (`ggml-common.h`, `block_q8_0`). There is no second scale term and no
interleaving to get wrong.

Every other layout (`Q4_0`, `Q4_1`, `Q5_0`, `Q8_1`, `Q4_K`, `Q6_K`, `MXFP4`, …)
is refused, by name:

```
no F32/F16/BF16/Q8_0 1-D or 2-D tensor in this file.
Unsupported quantised tensors: Q4_K, Q4_0, …
Re-export with llama.cpp --convert-f16 and import again.
```

Re-export from llama.cpp with `--convert-f16` if you need a file this tool can
read. Decoding the remaining block formats would mean re-implementing a dozen
quantisation kernels to throw the result away — the output is ternary either
way.

`tools/gguf_test.py` proves the `Q8_0` path rather than asserting it: a
hand-built file with four different per-block scales is compared against
`d * q` computed independently with `struct`, and a `Q8_0` fixture and the
byte-equivalent `F16` fixture are converted and required to produce the same
`.hydra`. Both directions of that test were checked against deliberately
broken decoders, because a test nobody has seen fail is not a test.

---

## 5. Hard limits — and why they are the ceiling, not a bug

The v1 format is a **fixed 24-byte header**, so the shape is bounded by
compile-time constants shared with the C engine:

| Constant | Value | Consequence |
|---|---|---|
| `HYDRA_EMBED_DIM` | 64 | dim ≤ 64 |
| `MAX_VOCAB` | 1024 | vocabulary ≤ 1024 entries |
| `MAX_LAYERS` | 4096 | at most 4096 layers per plane |
| bytes on disk | **2 bits per weight** | two weights per byte, one byte per `(layer, dim)` pair |

**The v1 ceiling is therefore 256 KiB of weights** (`64 × 4096` bytes =
262 144 bytes). A 20 GB checkpoint is ~81 920× larger than anything the current
format can describe, no matter how the converter is written. That is a format
limit, and `docs/FORMAT.md` carries the v2 plan (4-weights-per-byte packing,
64-bit offsets) that lifts it.

**RAM is not the blocker.** A 5 GiB sparse `.hydra` maps and generates fine
(`tests/test_large_model.c`). The obstruction is the header, not memory.

**Folding.** A `dim × rows` matrix is summed down its rows into a `dim`-wide
column profile, then rescaled so its peak is ±127 (`PEAK`), because the folded
magnitude becomes the layer count and `MAX_LAYERS` is 4096. Both planes are
rescaled by their own peak, so `--max-rows` changes the profile but not its
direction — which is the only thing the token decoder reads.

**Vocabulary.** The header field is a `uint16` capped at 1024, so a 128k BPE
vocabulary is truncated. The report states how many entries were dropped
(`droppedAboveMax`) rather than silently slicing.

---

## 6. Command line

```
python3 tools/gguf_to_hydra.py INPUT --out OUT [options]

  --out PATH          where to write the .hydra          (required)
  --json              machine-readable report
  --dim N             target dim (default 64, the format maximum)
  --max-rows N        use only the FIRST N rows of each tensor (default 4096, 0 = all)
  --a-tensor NAME     GGUF tensor for plane A   (default: the embedding)
  --b-tensor NAME     GGUF tensor for plane B   (default: the next decodable tensor)
  --vocab N           force the header vocab size (default: taken from the file)
  --vocab-out PATH    also write the vocabulary as JSON for the chat UI
  --expect PATH       write an .expected.json sidecar with A and B
```

`--expect` is what makes the result checkable by something other than itself:
`tools/gguf_test.py` recomputes `A` and `B` from the **packed bytes** with an
independent implementation and compares them value for value.

### "The next decodable tensor" — what that actually means

`--b-tensor` defaults to the first 2-D tensor **after** plane A that the reader
can decode, and "decodable" is a precise list, not a guess
(`tools/gguf_to_hydra.py`, `pick_tensors()`):

- **F32, F16, BF16** — read directly, and
- **Q8_0** — a *quantised* type, dequantised at read time (F16 scale `d` plus
  32 int8 per block, `w = d*q`).

So "next dense tensor" was wrong twice over: it excluded Q8_0, which is the one
quantised type the importer handles, and it implied a float tensor rather than a
decoding path. Anything outside that list — Q4_0, Q4_K, MXFP4 and the rest — is
**refused by name**, never silently skipped, so an import either uses a tensor
it really understands or says which type stopped it.

If the GGUF has no second decodable tensor, plane B is built from plane A's
columns rolled by one, and the report says so (`"columns rolled by 1 - no second
decodable tensor"`).

### `--max-rows` is a prefix, not a sample

`--max-rows` takes the **first** N rows of each tensor
(`used_rows = min(rows, max_rows)`, then `_row_blocks()` walks
`0 .. used_rows`). It does **not** pick rows at random or spread them evenly.
The default is **4096**; `0` means every row.

The distinction matters because the scale is derived from the rows that were
read: `mean_abs()` averages over exactly `used_rows`, so the gamma the weights
were ternarised against describes the same matrix the weights came from. On a
32 000-row embedding the default therefore folds rows `0..4095` and **discards
the rest** — a deliberate, stated truncation, not a representative sample of the
tensor.

---

## 7. HTTP routes

| Route | Behaviour |
|---|---|
| `POST /api/models/import?name=x.gguf` | streams to a temp file, converts, validates with the **same** header analysis as every other path, deletes the source GGUF, returns `{ok, model, conversion}` |
| `GET /api/models` | lists the result; models under `models/converted/` are flagged `converted: true` so the chat selector can mark them |

The converted file is validated against the engine's own header rules before it
is offered. A converter that produced something the engine would refuse is a
**500, not a model**: the file is deleted and the reason is returned.

The source `.gguf` is deleted after conversion. Only the `.hydra` ever reaches a
model path.

The 413 path is shared with upload: over `HYDRA_MAX_UPLOAD` the server answers
with a body naming the limit and how to raise it — never a dropped connection.

---

## 8. Verifying a conversion

```bash
python3 tools/gguf_test.py     # synthetic GGUF in, real C engine out
```

The gate is deliberately **not** a self-check. It builds a GGUF fixture,
converts it, loads the result with the **actual C engine**, and compares the
token sequence. A converter that emitted the wrong bit layout would still pass a
round-trip against itself; it would not survive the engine.

---

## 9. Honest limitations

- **Not a Llama.** See §2. The dominant token behaviour of the original network
  is not preserved.
- **Small.** 64 dimensions, ≤1024 vocabulary, ≤256 KiB of weights.
- **Float types, plus `Q8_0`.** `F32`, `F16`, `BF16` and `Q8_0` are decoded;
  every other block layout is refused by name, not guessed.
- **Vocabulary is truncated** to the format ceiling; the count of dropped
  entries is reported.
- **`A`/`B` are column sums.** Column sums discard the per-row structure that
  attention depends on. That is inherent to what the format stores, not a
  limitation of the fold implementation.
