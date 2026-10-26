# GGUF Import

How a `.gguf` checkpoint becomes a `.hydra` model — and, more importantly, what
that conversion does **not** do.

Read this before assuming a converted file is "your model, but ternary".

---

## 1. The short version

```bash
# read-only: what is in this file?
python3 tools/gguf_inspect.py model.gguf

# convert a GGUF -> .hydra (version 1, 2 ternary weights per byte)
python3 tools/gguf_to_hydra.py model.gguf --out models/converted.hydra

# convert a safetensors file -> .hydra (version 2, 4 ternary weights per byte)
python3 tools/safetensors_to_hydra.py model.safetensors --out models/converted.hydra

# or through the web console
POST /api/models/import?name=model.gguf
```

Python **standard library only** — no numpy, no torch, no pip install. The
GGUF converter streams, so it does not have to hold the whole tensor in
memory. The safetensors path is a second, independent converter (§10) that
emits the version-2 packing.

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

**Why the code compares against `mean|W| / 2` instead of rounding every
element.** The formula above is what the report describes; the implementation
(`ternarise_column_sums`) counts, per column, how many weights satisfy
`w >= mean/2` (the +1 count) and `w <= -mean/2` (the −1 count), because the
two forms are equivalent:

```
round_half_away(gamma * w) >= 1   <=>   gamma * w >= 0.5
                                  <=>   w >= 0.5 / gamma
                                  <=>   w >= mean(|W|) / 2
```

and `clip(·, −1, +1)` maps everything above the threshold to +1 regardless of
how far above. The same identity is the one the b1.58 literature states for
its own reference code (a weight's ternary code is nonzero exactly when
`|w| > mean|W|/2`), and it is what makes the whole quantisation two counters
per column — the ternary matrix is never materialised, which is what keeps a
4 GB download inside a phone's RAM. One float-level caveat, stated rather than
hidden: the comparison `w >= mean/2` and the hypothetical elementwise
`round(gamma*w) >= 0.5` can differ by one ulp for weights sitting exactly on
the threshold (because `fl(1/m) * m` is not exactly 1). That is a measure-zero
case for real weight distributions and does not change any column sum a
converted model in this repository stores.

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
Unsupported quantised tensors: blk.0.ffn_gate.weight (Q4_K), …
Re-export with llama.cpp --convert-f16 and import again.
```

The refusal names every refused tensor **with its layout** (`name
(TQ1_0)`), not just the name: the type table is audited against the
official `ggml_type` enum (see §11), and a message that says only
"token_embd.weight" would force the user to hexdump the file to find
out which quantisation it has.

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
| bytes on disk | **4 bits per weight** | two weights per byte (one per plane), one byte per `(layer, dim)` pair |

**The v1 ceiling is therefore 256 KiB of weights** (`64 × 4096` bytes =
262 144 bytes). A 20 GB checkpoint is ~81 920× larger than anything the current
format can describe, no matter how the converter is written. That is a format
limit. The 4-weights-per-byte packing already exists in the loader as header
version 2 (see `docs/FORMAT.md`, "Version 2") and halves the file for the same
model, but it does not raise the ceiling: the header's 32-bit fields and the
256 KiB bound are the same. 64-bit offsets live in the `.hydra2` container
spec (`docs/FORMAT-V2.md`) and remain unimplemented there, as stated in that
document.

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
- **The `.hydra2` container is not a GGUF loader** (2026-10-26). GGUF has two
  supported destinations only: the app's llama.cpp engine (full fidelity) and
  this offline converter (projection). Dropping a GGUF into the v2 container
  is refused with the GGUF magic named; the converter never writes `.hydra2`
  (it emits v1, or `--v3` for the RoPE-capable variant). See §11.
- **Little-endian only.** The GGUF spec allows big-endian files but provides
  no way to detect them; the spec says to assume little-endian, which is what
  the reader does (§11).

---

## 10. safetensors import (version-2 `.hydra`)

`tools/safetensors_to_hydra.py` is a second, independent converter for the
`safetensors` container (the format Hugging Face ships). It shares the
engine's absmean ternarisation and the same honesty rule as the GGUF path:
the result is a projection into the recurrent core, not the source network.

```bash
python3 tools/safetensors_to_hydra.py model.safetensors --out m.hydra
python3 tools/safetensors_to_hydra.py model.safetensors --out m.hydra --json
```

### Container parsing and validation

A safetensors file is `[8-byte LE u64 N][N bytes of JSON header][data
region]`. The JSON maps each tensor name to `{dtype, shape,
data_offsets:[start, end]}`, where the offsets are **relative to the data
region** (absolute range `[8+N+start, 8+N+end)`). Every tensor is validated
at **both** ends before any decode: `start >= 0`, `end > start`,
`8+N+end <= file_size`, and `end − start == n_elements × bytes_per_element`.
A header length past the file end, an unparseable header, a 3-D tensor,
out-of-order offsets, a span that disagrees with the shape, or a range that
runs past the file end is refused with a message that names the field and
the bound — never a generic "invalid file".

### Float types

`F32` (struct), `F16` (IEEE binary16) and `BF16` (the top 16 bits of an F32)
are decoded. Every other dtype (`Q8_0`, `F64`, …) is refused **by name**, not
guessed. This is a deliberate, smaller supported set than the GGUF reader
(which also decodes `Q8_0`); safetensors models are expected to export dense
floats.

### Ternarisation and the version-2 output

Each chosen plane is ternarised with the same b1.58 absmean rule as §3
(`gamma = mean|W|`, `W_q = clip(round(W/(gamma+1e-8)))`, half-away-from-zero;
the `+1e-8` keeps an all-zero tensor finite). The ternary column sums are two
counts per column (the threshold form of the rounding), so the ternary matrix
is never materialised. The column sums are folded to the 64-wide state and
written as a **version-2** `.hydra` — 4 ternary weights per byte, two
`dim/4`-byte planes per layer (`docs/FORMAT.md`, "Version 2"). The writer
stores the folded sums as layer sums of `±1`, so `sum_l w1[l][i] == A[i]`
value for value — the first shipped writer for the version-2 packing.

### Verification

`tools/gguf_test.py` gates the path end to end (`run_safetensors`), proven
three independent ways: an independent byte reader re-derives `A`/`B` from
the packed bytes; an oracle written from the literal round-clip rule must
equal the reported `A`/`B`; and the real `./hydra-run` on the version-2 file
matches a reference step. F32/F16/BF16 of the same weights convert
identically, and a set of malformed inputs (truncation, header length,
out-of-order offsets, unsupported dtype, past-the-end range) must each be
refused.

---

## 11. Verification (2026-10-26)

This round re-verified the whole GGUF surface against the official
sources and closed the gap between "parses" and "actually works".

### The two GGUF paths in this repository

| Path | What it is | Fidelity |
|---|---|---|
| **The app's llama.cpp engine** (`android/app/src/main/java/dev/hydrastone/LlamaEngine.kt` → `llama_jni.cpp` → pinned llama.cpp) | full transformer inference: real attention, real vocabulary, real sampling | **full** — any GGUF the pinned build accepts is run as that model |
| **the offline converter** (`tools/gguf_to_hydra.py`) | projection of the GGUF weights into the 64-dim recurrent `.hydra` core | **seed only** — §2; not the source network |

The app's question "can GGUF models be used, and do they write with
their full intelligence?" is answered by the llama.cpp engine: it is
the app's main engine (`isExperimental = false` per
`LlamaEngine.kt`), loads `.gguf` files imported through the Models
tab (SAF copy, GGUF magic check, size cap in `MainActivity.kt`), and
delegates "which architectures work" to llama.cpp's own loader
instead of re-implementing a support matrix that would drift. The
conversion path in this document is **not** that answer, and the two
paths refuse each other's files at the magic boundary (§11.6).

### 11.1 ggml_type enum audit — bug found and fixed

The reader's quantised-name table disagreed with the official
`ggml_type` enum (ggml-org/ggml, `include/ggml.h`, fetched
2026-10-26): the table claimed `31 = TQ1_0`, `32 = TQ2_0`,
`34 = MXFP4`. Officially, `TQ1_0 = 34`, `TQ2_0 = 35`, `MXFP4 = 39`,
with `NVFP4 = 40`, `Q1_0 = 41`, `Q2_0 = 42` missing from the table
entirely, and slots `31–33` / `36–38` REMOVED (reserved) — they must
not be given invented names. Impact: a TQ1_0 file would have been
refused under the label `MXFP4`. Fixed in `tools/gguf_reader.py`;
the regression gate `run_type_names` in `tools/gguf_test.py` now
checks all six current names plus all six reserved slots, and the
converter refusal names tensor **and** layout
(`token_embd.weight (TQ1_0)`, §4).

### 11.2 Spec conformance (ggml-org/ggml, `docs/gguf.md`)

Verified against the official GGUF spec on the same date:

- header order (magic `0x46554747` LE, version, tensor_count,
  kv_count), all 13 metadata value types, tensor info (name,
  n_dims ≤ 4, dims, ggml_type, offset **relative to the data
  section**), alignment handling and the end-of-file bound check all
  match; the reader accepts format version 2–3 and refuses others.
- the reader is **little-endian only**. The spec allows big-endian
  GGUF but states there is currently no way to detect it, and says
  to assume little-endian without further information — exactly what
  the reader does.
- the spec caps tensor names at 64 bytes; the reader does not enforce
  the cap (an overlong name is malformed input but harmless here).

### 11.3 Behavior gate: the converted model actually writes

`run_behavior` in `tools/gguf_test.py` (CI-gated through `make
gguf-test` inside `test-run`): the converted fixture model must
produce a **deterministic** stream (two independent `./hydra-run`
runs, byte-identical) and a **non-degenerate** stream (≥2 distinct
tokens, not the input token repeated) for all three tested
(token, steps) pairs. That is the honest ceiling of what a
projection can prove: the import produces a stable, alive model — it
does not claim the stream reproduces the source LLM's language.

### 11.4 llama.cpp smoke test (the pin, not a newer build)

`tools/gguf_minimal_llama.py` (standard library only, deterministic
fixed-seed LCG weights, 2 layers, 64 dim, 99-token BPE vocabulary,
F16) writes a structurally real `llama`-architecture GGUF — every
field llama.cpp's llama loader reads is present. The `gguf-test`
gate keeps the generator alive by requiring this repository's own
reader to parse it (`run_minimal_llama`). The generation step runs
against the **pinned** llama.cpp — v0.5.0, commit
`7fe450e19305b828c199d602c23a8337aaa1f03b`, the same commit the
app's submodule pins and the CI `llama.cpp pin` check enforces:

```bash
# pinned source (the codeload tarball is the no-clone way):
curl -sL -o /tmp/llama.tar.gz \
  https://codeload.github.com/ggml-org/llama.cpp/tar.gz/7fe450e19305b828c199d602c23a8337aaa1f03b
tar xzf /tmp/llama.tar.gz && cd llama.cpp-7fe450e*
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF
cmake --build build --parallel 2 --target llama-cli
python3 tools/gguf_minimal_llama.py --out /tmp/smoke.gguf --seed 7
./build/bin/llama-cli -m /tmp/smoke.gguf -p "a" -c 64 -n 4 -t 1 --temp 0
```

Result on this machine (2026-10-26, 2 cores, `OMP_NUM_THREADS=1`):

```
> a

[ Prompt: 36894.7 t/s | Generation: 6237.0 t/s ]
```

The pin loads the file our generator wrote, tokenizes, decodes and
samples — the full pipeline, on a file that needed no download. The
generated text is empty by construction: random weights point at
control/special tokens, and that is the ceiling of a fixture (the
generator's docstring says so). Full-intelligence GGUF inference in
the app is the llama.cpp engine's job with a real model file.

Three build-specific facts came out of that run and are baked into
the generator (all regression-covered by `run_minimal_llama` and the
generator's own self-test):

- `tokenizer.ggml.model` must be a llama.cpp vocab type — `"gpt2"`
  for a BPE charmap, not `"BPE"` (the loader answers "unknown
  tokenizer").
- `tokenizer.ggml.token_type` must be an **INT32** array.
- the llama arch in this build wants `ffn_gate`/`ffn_up` as
  `{n_embd, n_ff}`, `ffn_down` as `{n_ff, n_embd}`, plus
  `output_norm.weight` and `llama.feed_forward_length`.

Two operational notes: this build's `llama-cli` always starts its
interactive REPL, and with a **closed** stdin (a bare pipe) its
server loop busy-spins printing `> ` prompts — run it with stdin
open (or timeboxed) to capture the one-shot output. And the F16
variant of this fixture reaches a ggml type mismatch in the pinned
build (f32 activations × f16 norm weights in `ggml_mul`); `--dtype
f32` (the default) is the verified smoke configuration. Real
llama.cpp conversions are usually quantised, so the fixture proves
structure and the pipeline — not fidelity.

### 11.5 What this round added to the gates

- `run_behavior` (determinism + non-degeneracy of the converted
  model's stream) — `tools/gguf_test.py`.
- `run_type_names` (ggml_type audit regression) — `tools/gguf_test.py`.
- `run_minimal_llama` (the generator's file must still parse) —
  `tools/gguf_test.py`.
- the `.hydra2` reader now names a GGUF magic in its refusal (2 new
  checks in `tests/test_hydra2_container.c`; the suite is 122
  checks).

### 11.6 Format boundaries

- `.hydra2` is **not** a GGUF loader; a GGUF dropped in is refused
  with the GGUF magic named (`src/hydra_v2_container.c`, regression-
  tested, §11.5).
- the offline converter never emits `.hydra2`: GGUF import writes
  v1 (or `--v3`, the RoPE-capable variant), and `.hydra2` fixtures
  come from `tools/make_model.py --v2`.
