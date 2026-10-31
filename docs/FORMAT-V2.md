# The `.hydra2` Container Format (v2)

> **Status: SPECIFICATION, container implemented.** The container reader
> (`src/hydra_v2_container.c`), the fixture generator
> (`tools/make_model.py --v2`) and the test suite
> (`tests/test_hydra2_container.c`) exist and are gated in CI. What does
> **not** exist yet: MoE inference on top of the container, an expert cache,
> and any measured throughput (section 6 is still a worked example, not a
> result). See "Implementation status" below for the exact boundary.
>
> **v1 is untouched.** Every `.hydra` v1 file written by any v1 release loads
> under any v1 loader, and no v2 change alters that. v1 and v2 have different
> magics, different version fields and different file extensions
> (`.hydra` vs `.hydra2`). A v1 loader must refuse a v2 file loudly, not read
> it partially. The v1 loader does refuse it: `hydra_engine_load()` checks the
> 8-byte v2 magic before the v1 magic check, because the first four bytes of
> `HYDRA2\0\0` are the v1 magic `HYDR` (`[CODE]` src/hydra_engine.c).
>
> **GGUF is not a v2 container either** (2026-10-26): the v2 reader names the
> GGUF magic (bytes `0x47 0x47 0x55 0x46`, LE u32 `0x46554747`) in its
> refusal and points at the two supported GGUF destinations — the app's llama.cpp engine (full fidelity)
> and `tools/gguf_to_hydra.py` (the projection, `docs/GGUF-IMPORT.md`). Two
> regression checks in `tests/test_hydra2_container.c` gate the code and the
> message.
>
> **Disambiguation, because the name "version 2" is used two ways here.**
> This document's v2 is the `.hydra2` container: magic `HYDRA2\0\0`, a
> 128-byte header, sparse-MoE. The v1 loader in `src/hydra_engine.c`
> separately accepts a `version == 2` value inside the ordinary 24-byte
> `HYDR` header — a 4-weights-per-byte packing variant of the v1 format
> (documented in `docs/FORMAT.md`, "Version 2"). That is NOT this container:
> different magic, different header, no experts, no streaming. Statements in
> this document about a "v2 reader" refer to the `.hydra2` container reader
> in `src/hydra_v2_container.c`.

## Roadmap status — this is THE canonical v2 spec

Two different designs have been called "v2" in this repository, and they are not
the same thing. Rather than quietly reconciling them, the disagreement is
recorded here:

| Design | Where it is described | Status |
|---|---|---|
| **Sparse-MoE `.hydra2`** — trunk plus streamed experts, `top_k` routing, an expert cache, a bounded resident set | **this document** | **CANONICAL.** This is the v2 specification of record. |
| Aggregated container holding only `A[i]`/`B[i]`, flagged `HYDRA_AGGREGATE_V2` | `docs/ARCHITECTURE.md` § *Known Structural Redundancy*, `docs/FORMAT.md` § *Planned Extensions (v2)* | **SUPERSEDED exploratory sketch.** Not a competing spec. See below. |

### Why the aggregated container is superseded, not rejected

The aggregated container is a **real, correct observation**: v1's layer loop is
algebraically redundant, so a v1 model compresses losslessly into `A[i]/B[i]`
(see `docs/ARCHITECTURE.md`). It is simply **not the format to build next**, for
one reason: `A[i]/B[i]` is exactly what the v1 loader *already computes at load
time* and discards on every call. An aggregated container would ship a file that
holds nothing the running engine does not already have in ~1.1 KiB of RAM
(`docs/ARCHITECTURE.md` § *Memory Model*). It would halve the file size and
change nothing about capability.

The sparse-MoE design, by contrast, is the only one that changes **what is
possible**: it makes models larger than RAM viable, because weights that are not
resident are fetched on demand instead of demanded up front. That is the whole
reason v2 exists (see §1), and the brief for the format is
[`.agents/docs/HYDRA2-RESEARCH.md`](../.agents/docs/HYDRA2-RESEARCH.md).

**So:** the two passages above are kept, and clearly marked, rather than deleted
— deleting a documented observation because it lost an argument is how a
repository stops being able to explain why a design was rejected. Anyone
implementing "v2" implements **this** document. The aggregated container stays
available as a possible *v1.1 file-level optimisation* if it is ever wanted, and
that is the only sense in which it is still on the table.

### Implementation status of this document

Stated precisely, with the boundary between implemented and not:

- **Implemented `[CODE]` (2026-10-26):** the container reader
  `src/hydra_v2_container.c` — the 128-byte header parser with the header
  CRC-32 verified at open, both-ends validation of every region and entry
  (section 5), the metadata K/V block, the tensor directory, the expert
  directory (order + contiguous-offset rules), `hydra2_read_tensor()`
  (trunk), `hydra2_read_expert()` (one `pread` per expert, CRC-32 verified
  at read time, 5.1.5), and `hydra2_route_top_k()` (deterministic top-k with
  tie-to-lower-id and softmax over the selected logits). The fixture writer
  `tools/make_model.py --v2` produces files the reader accepts, and
  `tests/test_hydra2_container.c` (run as `make hydra2-test`, gated in CI)
  covers the happy path, every refusal class, routing determinism, the
  Python/C cross-check, and the mutual v1/v2 refusal (7.2/7.3).
- **Implemented `[CODE]` (2026-10-27):** the trunk is **memory-mapped**
  when the kernel allows it — a page-aligned `mmap(MAP_PRIVATE, PROT_READ)`
  range covering the whole trunk, tensor reads are `memcpy` out of the
  mapping (`src/hydra_v2_container.c`, `trunk_backend` field: `"mmap"` or
  `"pread"`). The explicit opt-out `HYDRA2_NO_MMAP=1` and a kernel refusal
  both fall back to the original one-`pread`-per-tensor path, byte-for-byte
  (parity CRC tested in `tests/test_hydra2_container.c`,
  `test_trunk_mmap_backend`). Expert streaming is deliberately unchanged:
  one `pread` per expert, CRC-32 verified at read time (5.1.5).
  The mapped extent is remembered in `trunk_map_len` so `hydra2_close()`
  munmaps the exact range; round 13 (Qodo #5) fixed the pre-fix zero
  length that made the munmap a no-op and leaked the mapping until
  process exit (the CI sanitizer gate now runs LeakSanitizer enabled).
- **Implemented `[CODE]` (2026-10-27):** `hydra-run --inspect <file>`
  prints a JSON summary of a `.hydra2` file (version, header bytes, the
  five regions, routing metadata, metadata key/value pairs, the tensor
  directory, up to eight sample experts, and the reader backend) with the
  same exit-code contract as the v1 inspection (0 ok / 2 reader refusal /
  1 usage). The web server's `/api/models` and inspect route accept
  `.hydra2` files (`server.js`, async `execFile` on the binary's exit
  status — the pre-fix sync spawn blocked the whole event loop for up to
  the 10 s timeout, round 13 Qodo #27 — covered by
  `tools/server_test.js`), and the `tools` frontend lists and displays
  them. The inspect JSON emits every metadata value as a COMPLETE JSON
  value (numbers bare, strings and hex blobs quoted by the encoder;
  round 13, Qodo #18 fixed the pre-fix first-character heuristic that
  double-quoted binary values starting with a non-`0` hex digit), and a
  failed read refuses WITHOUT echoing the path (round 13, Qodo #25).
- **Implemented `[CODE]` (2026-10-30, round 12 stages 4–6):** MoE
  inference on top of the container — `src/hydra_moe.c` /
  `include/hydra_moe.h`, engine `moe-transformer-v1`: per-layer GQA
  attention (RoPE, sink+ring KV caches), router → top-k → streamed expert
  FFN (SwiGLU), final RMSNorm, real per-vocabulary logits, and a
  min-p / repetition-penalty sampler over them. `hydra-run` dispatches on
  the file magic: `.hydra2` files run the MoE engine (v1–v4 files run the
  legacy engine, bit-identical, untouched). Expert layouts: **0** = flat
  F32 and **1** = TERNARY2 + per-group F32 scales (section 3.2.2);
  **trunk** tensors of the block-scaled ternary types are materialized
  through their paired `<name>.scale` sibling at read time (section
  3.2.1), and the optional **role-8 vocabulary** (`vocab` / `merges`,
  section 3.2.3) carries the token strings: the module validates them at
  open (UTF-8, uniqueness, merge targets) and exposes the BPE codec
  (`hydra_moe_encode_text` / `hydra_moe_decode_token`), which
  `hydra-run --text-prompt` uses (loud refusals: no vocab, v1 file,
  unmapped byte, invalid UTF-8, overflow, flag combination). The v1
  legacy engine and its pinned streams are not modified. The CLI prompt
  CONDITIONS the model since round 13 (Qodo #30): every prompt token
  except the last is run through the engine to build the KV history and
  generation continues from the LAST prompt token (pre-fix the prompt
  only seeded the repetition ring and the display). The generated stream
  is reported as decoded text as well as token ids (`[Text-out]:` line
  and the JSON `generated_text` field, round 13 Qodo #7). Covered by
  `tests/test_hydra_moe.c` (`make test-run`, 180 checks: independent-
  reference forward parity, greedy stream pins for the F32 / S64 / S128 /
  dim-256 multi-group / attention-ternary / GQA fixtures, KV eviction,
  sampler extremes incl. the temperature-scaling pin, the hand-crafted
  dequant round-trips (the in-place aliasing regression of round 13
  Qodo #3 included), the vocab codec + tamper refusals, the
  32-bit-wrap / non-divisible-GQA shape-tamper refusals, and the
  mismatched-KV-dim refusal), by
  `tests/test_hydra2_container.c` (`test_vocab_types`) and by
  `tools/text_prompt_check.sh` (CLI gate).
  **Not implemented:** the expert cache / slot leasing of section 6.2,
  prefetch of section 6.4, and every `B_eff`/throughput number in section
  6, which remain **NOT MEASURED**. (The JNI bridge's batch sampling API
  has, since 2026-10-27, transformer v3/v4 coverage in
  `tests/test_jni_batch.c` — batched runs equal direct runs for greedy
  and fixed-seed sampling; that is the v3/v4 engine, not a `.hydra2`
  consumption path.)
- **Implemented `[CODE]` (2026-10-30, round 13):** engine hardening from
  the Qodo audit of PR #73 — the MoE sampler's temperature now scales
  the EXPONENT, `exp((v-mx)/T)`; the pre-fix `exp(v-mx)/T` + renorm
  cancelled T entirely, so every temperature sampled the same
  distribution (Qodo #8, pinned by a three-temperature CDF test). The
  repetition penalty walks the WHOLE window (pre-fix it was capped at
  the vocabulary size, Qodo #21); the stochastic path requires a
  non-NULL RNG state (greedy may pass NULL, Qodo #19). The UTF-8
  validator returns a distinct "invalid" code with the error offset —
  the pre-fix `0` conflated "valid" with "invalid at offset 0" (Qodo
  #20). The CLI repetition ring is a true circular buffer: the pre-fix
  write position was clamped at the window, so after the first
  wraparound every write hit slot 0 and `--rep-window 0` divided by
  zero (Qodo #16/#22, pinned by `tools/text_prompt_check.sh`). The
  positional-argument defaults/clamp (start 42, steps 1..256) now
  precede the magic dispatch so `.hydra2` files get them too (Qodo
  #23). All regression tests are in `tests/test_hydra_moe.c` and
  `tools/text_prompt_check.sh`; the suite grew 138 -> 180 checks.
- **SIMD status (2026-10-26).** The transformer formats v3/v4 (the engine's
  per-layer ternary projections) compute their inner product through
  `tf_row_dot`, which is **integer-exact** and has AVX2 and NEON kernels
  (`src/hydra_simd.h` / `src/hydra_neon.h`, dispatched in
  `src/hydra_engine.c` — `AGENTS.md` §2.1): x86 and ARM runs of a v3/v4
  model are bit-identical token streams, pinned by parity tests. Two
  deliberate scope decisions: the container's own arithmetic — the expert
  top-k routing (`hydra2_route_top_k`, O(k²·n) with a double softmax) — is
  scalar **by design**, because it is selection/ordering without
  data-parallel arithmetic (SIMD has no correct form there), and the float
  parts of v3/v4 (RMSNorm/RoPE/SwiGLU) stay scalar so the pinned token
  streams are not broken by reassociation.
- The **Q8_0 decoder** that appears in §3.2 is real code, but it belongs to the
  **GGUF import path** (`tools/gguf_reader.py`, feeding
  `tools/gguf_to_hydra.py`). It is a Python reader for somebody else's file
  format that converts to a **v1** `.hydra`. The v2 container validates Q8_0
  *entries* (element-group and byte-size rules) but does not **decode** Q8_0
  values; the same is true for F16/BF16. The types the current test
  fixtures exercise are F32, TERNARY2_S64/S128 (expert layout 1 and
  attention trunk tensors) and the role-8 pair VOCAB_UTF8/U16_PAIRS
  (section 3.2.3).
- The **research** that motivates the sparse-MoE path is in
  [`.agents/docs/HYDRA2-RESEARCH.md`](../.agents/docs/HYDRA2-RESEARCH.md). Its benchmark plan is
  explicitly **not yet run**; nothing in it may be quoted as a result.

## Provenance rule for this document

Every claim below carries one of three labels:

- **[CODE]** — a symbol in this repository that a reader can check.
- **[SPEC]** — a requirement of this format, not yet implemented.
- **[MEASURED]** — a measurement, with the machine and date named.
- **[ESTIMATE]** — an explicit projection with its assumptions written out.
- **[NOT MEASURED]** — a quantity that has not been measured and must not be
  presented as if it had been.

---

## 1. Why a second version

v1 describes a recurrent ternary model whose weights are aggregated **once at
load** into `A[i]` and `B[i]` by `hydra_build_aggregation()` in
`src/hydra_engine.c`; the per-token `hydra_engine_step()` then executes
`2*dim` multiply-accumulates and never reads the weight region again. **[CODE]**

That design is the right one for its problem, and the problem is small by
construction: v1 caps weights at 256 KiB (`dim <= 64`, `layers <= 4096`,
`vocab <= 1024`). **[CODE]** `include/hydra_model.h`.

v2 is a different thing: a **sparse mixture-of-experts** container in which the
bulk of the parameters are *not* resident and are streamed from storage per
token. Two consequences force a new version rather than a new option:

1. **v1's offsets are 32-bit.** A sparse model larger than 4 GiB cannot be
   described by a `uint32` offset. v2 uses `uint64` throughout. **[SPEC]**
2. **v1's payload is a single contiguous block.** v2 needs a directory of
   independently-seekable, independently-checksummed payloads, because the
   whole point is reading a few of them per token. **[SPEC]**

---

## 2. Design reference

`github.com/JustVugg/colibri` is a design reference for the *approach* —
treating storage, RAM and cache as one placement hierarchy, reading each
expert with one `pread`, batching the union of experts a batch needs, and
letting measured routing heat decide residency. No code is copied. **[CODE]**

Facts about that project used below, with their qualifications kept intact:

| Fact | Label | Qualification |
|---|---|---|
| GLM-5.2: ~9.9 GB resident dense weights | **MEASURED by Colibri**, README fetched 2026-10-03 | Their number, on their machine, for their container |
| 19,456 routed experts, ~19 MB each at int4, ~370 GB on disk | **MEASURED by Colibri** | 75 MoE layers x 256 + the MTP head |
| 16 GB min / 24 GB comfortable system RAM | **SPEC of their container** | **This is system RAM, not dense-weight size** |
| 71.6% routing predictable one layer ahead | **MEASURED by Colibri** | **Next-layer routing predictability, NOT an operational cache-hit rate** |
| 12.7 GB of routed experts per token (GLM-5.2) | **MEASURED by Colibri** | The number that actually bounds throughput |
| 0.05–0.1 tok/s cold on a 25 GB dev box | **MEASURED by Colibri** | Their words: "the proven floor" |

---

## 3. Supported architectures and tensor types (explicit)

**This format supports exactly the architectures in the table below and
nothing else.** A reader must reject anything not on that list, by name, and
must not guess. **[SPEC]**

### 3.1 Architectures

| Architecture | Status in v2 |
|---|---|
| `hydra-recurrent-v1` (this repository's recurrence) | **required** — the reference implementation |
| `moe-transformer-v1` (decoder-only transformer with per-layer MoE FFN) | required |
| Anything else | **rejected by name**, with the name echoed back |

Rejection is not a warning. A file whose `general.architecture` is not in this
table is refused before a single tensor directory entry is read. **[SPEC]**

### 3.2 Tensor element types

| Type id | Name | Bytes/element | Notes |
|---|---|---|---|
| `0x0000` | `TERNARY2` | 0.5 | two 2-bit ternary weights per byte, identical packing to v1 **[CODE]** |
| `0x0010` | `F16` | 2 | IEEE binary16 |
| `0x0011` | `BF16` | 2 | top 16 bits of an F32 |
| `0x0012` | `F32` | 4 | |
| `0x0020` | `Q8_0` | 34/32 | 34-byte blocks: F16 `d` + 32 int8, `w = d*q` **[CODE]** — implemented in `tools/gguf_reader.py` for the GGUF import path, not yet in a C v2 reader |
| `0x0030` | `Q4_0` | 18/32 | reserved, **not implemented** |
| `0x0031` | `Q4_K` | reserved, **not implemented** |
| `0x0032` | `MXFP4` | reserved, **not implemented** |
| `0x0040` | `TERNARY2_S64` | 0.5 | TERNARY2 packing + per-64-element block scales, sections 3.2.1 / 3.2.2 **[CODE]** |
| `0x0041` | `TERNARY2_S128` | 0.5 | TERNARY2 packing + per-128-element block scales, sections 3.2.1 / 3.2.2 **[CODE]** |
| `0x0050` | `VOCAB_UTF8` | var (>= 2) | NUL-terminated UTF-8 token strings, one per token id, section 3.2.3 **[CODE]** |
| `0x0051` | `U16_PAIRS` | 4 | BPE merge rules `(left u16, right u16)` LE, section 3.2.3 **[CODE]** |

The v2 reader **validates** all seven implemented types structurally
(`n_elements` multiple of the element group, `n_bytes == n_elements *
bytes_per_element`; the variable-length pair follows its own rule,
section 3.2.3: `VOCAB_UTF8` needs `n_bytes >= 2 * n_elements` — at least
one data byte plus the NUL per token — and `U16_PAIRS` needs exactly
`4 * n_elements`) and **refuses** the last four **by name** rather than by
falling through to "unsupported" (`[CODE]` `src/hydra_v2_container.c`,
tensor/expert entry validation). Value **decoders** exist in this repository
in the sense that `tools/gguf_reader.py` can read F32/F16/BF16/Q8_0 for the
**GGUF import path**, which produces a **v1** `.hydra`, and the MoE module
(`src/hydra_moe.c`) decodes the expert payloads it streams: F32 (layout 0)
and TERNARY2_S64/S128 (layout 1, section 3.2.2). The container itself
still only streams expert bytes into caller buffers; the section 6.2 expert
cache and the 6.4 prefetch remain open. **[SPEC]** + **[CODE]**

#### 3.2.1 Block-scaled ternary (M2.2)

`TERNARY2_S64` and `TERNARY2_S128` are the block-scaled ternary formats.
Their **byte layout is the TERNARY2 layout itself** — two 2-bit codes per
byte, `n_bytes == n_elements / 2`, `n_elements` even — so the container
validates them with the TERNARY2 packing rule; the group size is *not* a
property of the byte stream. **[CODE]**

What the id adds is a **scale convention**: a tensor of one of these types
is stored *paired* with a plain `F32` tensor named `"<name>.scale"`, whose
`n_elements` is `ceil(n_elements / 64)` (S64) or `ceil(n_elements / 128)`
(S128) — the **last group may be partial**, the same convention as the
expert sub-matrices (section 3.2.2). The scale of group `g` is applied to
elements `g*G .. min(n, (g+1)*G)-1` during inference, fused as one float
multiply-add per group, left to right — the precision contract of
`hydra_tf_row_dot_scaled()` (`include/hydra_model.h`, implemented in
`src/hydra_engine.c`, parity-tested in `tests/test_engine.c`).
**[CODE]**

The **MoE module** (`src/hydra_moe.c`) materializes trunk tensors of these
types through the same paired convention (FORMAT-V2.md section 4.2 role
names `attn_q/k/v/o.{L}` and friends): on read it allocates the destination
width (`n_elements * 4` bytes), copies the `n_elements / 2` payload bytes,
locates the `"<name>.scale"` sibling (F32, exactly `ceil(n_elements / G)`
elements) and writes `value = code * scale` into the destination — after
which the engine runs the **identical F32 arithmetic**. A missing or
mismatched sibling tensor is a loud refusal (section 5.2), and a retyped
or mis-sized tensor is refused by the container's structural check.
`tools/make_model.py --moe-full --moe-attn-ternary [64|128]` writes such
files (flat group convention: a scale covers `G` consecutive elements in
row-major order), and the greedy stream of one such fixture is pinned in
`tests/test_hydra_moe.c`. **[CODE]**

The paired-tensor form is the **tensor-directory** convention (a trunk
tensor plus its `<name>.scale` sibling). The MoE expert stream uses the
same two type ids with a **self-contained blob** convention instead,
because expert directory entries have no sibling mechanism — see section
3.2.2. Both conventions share the TERNARY2 code table and the F32 scale
semantics. **[SPEC] + [CODE]**

#### 3.2.2 Ternary MoE experts (`v2.expert_layout` 1, 2026-10-30)

The MoE module (`src/hydra_moe.c`) understands two expert layouts,
selected by the `v2.expert_layout` metadata key (section 4.4). The
container stays structural: it validates the expert directory entries
(length, type, CRC, ordering) and does not interpret the payload bytes.
The layout rules below are enforced by the MoE module. **[SPEC] + [CODE]**

* **Layout 0 (F32 flat).** Every expert is `3 * inter * dim` F32 values,
  row-major: `gate (inter x dim) | up (inter x dim) | down (dim x inter)`.
  Every expert directory entry carries type `F32` and the same length.
* **Layout 1 (TERNARY2 + block scales).** Every expert is the
  concatenation of three sub-matrix blocks, in the order
  `gate | up | down`, and each block is

  ```
  [payload bytes][scale F32 array]
  ```

  for its `(rows x cols)` matrix (`gate`: `inter x dim`, `up`:
  `inter x dim`, `down`: `dim x inter`; row-major element order):

  * **payload** — the exact TERNARY2 packing, `n/2` bytes (`n = rows *
    cols`, which must be even): element `2j` in the LOW two bits of byte
    `j`, element `2j+1` in bits 2–3; code table `00 = 0`, `01 = +1`,
    `10 = -1`, `11 = 0 (reserved)` — the same table as v1
    (`hydra_decode_ternary_2bit`).
  * **scales** — `rows * ceil(cols / group)` F32 values, row-major
    `[row][group]`; one scale covers `group` consecutive columns of one
    row and the **last group may be partial**. `group` is 64 for entry
    type `TERNARY2_S64` (`0x0040`) and 128 for `TERNARY2_S128`
    (`0x0041`); all entries of one file must use the same type.
  * **dequantization** — `value = code * scale` (exact in F32: a multiply
    by −1, +1 or 0). The engine materializes the selected expert into a
    per-expert F32 buffer and then runs the **identical** layout-0 expert
    arithmetic (same mat-vec, same SwiGLU, same summation order). There
    is deliberately no separate ternary mat-vec kernel: at batch size 1
    the materialization is cheap, and the downstream arithmetic is
    provably the F32 path's, so the two layouts differ only in their
    stored weights.
  * **quantization recipe** (the fixture writer,
    `tools/make_model.py --moe-full --moe-ternary [64|128]`): per row
    group, `s = max |w|` of the group (F32, `0.0` for an all-zero group)
    and code `+1` when `w >= 0.5*s`, `-1` when `w <= -0.5*s`, else `0`
    (an all-zero group codes everything as `0`). The recipe is
    deterministic (fixed LCG seed) and the dequantized value of element
    `i` is exactly `code_i * s` in F32.
  * **size** — per expert: `Σ over the three sub-matrices of
    (n/2 + rows * ceil(cols/group) * 4)` bytes; the MoE module computes
    this from `v2.inter`/`v2.dim` and the entry type and refuses any
    entry whose `length` or `type_id` deviates. Example (dim 8, inter 4,
    group 64): gate/up `4x8` → 16 + 16 B each, down `8x4` → 16 + 32 B,
    total **112 B** versus 384 B for layout 0 (≈3.4×).
  * **metadata** — `v2.expert_layout = 1` and `v2.expert_bytes_typ` set
    to the layout-1 byte count.

The test suite pins the greedy KV token streams of one S64, one S128 and
one dim-256/inter-64 S64 (multi-group: four 64-element groups per
gate/up row) fixture (`tests/test_hydra_moe.c`), and the S64/S128
streams of the small fixture are identical by construction: there, every
row has fewer columns than the smallest group, so both group sizes yield
one full-row scale — the same quantized weights. **[CODE]**

#### 3.2.3 Embedded vocabulary + BPE merges (role 8, M4)

A `.hydra2` file may carry the token strings themselves, in the tensor
directory (role 8, `HYDRA2_ROLE_VOCAB`), so that a text prompt can be
encoded *without* an external tokenizer. Both tensors are **optional** —
an inference-only file without them is valid; the `--text-prompt` CLI
feature (`hydra-run --text-prompt`, see `--help` and the README) requires
`vocab`, and `merges` is only legal together with `vocab`. **[SPEC]**

* **`vocab`** — type `VOCAB_UTF8` (`0x0050`), `n_dims` 1,
  `n_elements == v2.vocab_size`. The payload is the token strings in
  token-id order, **each NUL-terminated** (so the blob ends in a NUL and
  holds exactly `n_elements` NUL bytes, `n_bytes >= 2 * n_elements`).
  Every string must be valid UTF-8 (RFC 3629: no overlong forms, no
  surrogates, nothing above U+10FFFF), and **no two tokens may share a
  string** — a duplicate would make the byte map and the merge targets
  ambiguous. **[CODE]**
* **`merges`** — type `U16_PAIRS` (`0x0051`), `n_dims` 1, exactly
  `4 * n_elements` payload bytes: the rules `(left u16, right u16)` in
  little-endian, in **priority order** (rule 0 is the highest-priority
  rule). Every rule must name tokens that exist, and its **target token
  must be the concatenation** `string(left) ++ string(right)` as a token
  that actually exists in the vocabulary — a rule without a target is a
  loud refusal at open time. `merges` without `vocab` is refused; `vocab`
  without `merges` is legal (no BPE, byte-level encoding only).
  `n_elements` may be 0 only by omitting the tensor (the directory
  requires `n_elements >= 1`). **[CODE]**

The **encoding** (`hydra_moe_encode_text`, `include/hydra_moe.h`) is
byte-level BPE with the GPT-2-style priority rule:

1. every input byte must map to exactly one single-byte token
   (UTF-8 input, one-byte-per-token map built at open time); an unmapped
   byte is refused (`-3`), invalid UTF-8 is refused (`-2`);
2. the sequence is then merged **repeatedly until a fixed point**: scan
   the rule list from rule 0; apply the first rule that matches, replacing
   **all** its occurrences in one pass, then restart the scan at rule 0.
   (Applying rule 0 may create a new match for rule 0 — the restart is
   what makes `abcd` → `[ab] [cd]` → `[5, 6]` with rules
   `(0,1)->5, (2,3)->6, (1,2)->7`.)
3. the sequence **never grows**, so it fits in a buffer of the input
   length; a request for fewer slots than input bytes is `-4`.

`hydra_moe_decode_token` maps a token id back to its string (the inverse
of step 1's table plus the merge strings). The fixture schema of
`tools/make_model.py --moe-full` is tokens `a..e` (0–4), `ab` (5), `cd`
(6), `bc` (7) and `tok%04d` (8+); the reference rules are
`(0,1)->5, (2,3)->6, (1,2)->7`. The C codec, the container type rules and
the CLI are parity/pin-tested in `tests/test_hydra_moe.c`
(`test_moe_vocab_codec`, `test_moe_vocab_tamper`),
`tests/test_hydra2_container.c` (`test_vocab_types`) and
`tools/text_prompt_check.sh`. **[CODE]**

### 3.3 Out of scope

Mamba / SSM / structured-state-space layers are **out of scope for v2**. They
are a separate research branch requiring their own evidence and approval.
**[SPEC]**

---

## 4. Container layout

All integers are **little-endian**, without exception. A big-endian host must
read through explicit byte-wise readers, exactly as v1 does
(`rd_u32le` in `src/hydra_engine.c`). **[CODE]**

**Alignment `[CODE]` (implementation decision, 2026-10-26).** The layout
diagram says regions are "aligned" without a value; the reader enforces
**16-byte** alignment for every region offset (`metadata_offset`,
`tensor_dir_offset`, `expert_dir_offset`, `trunk_offset`, `experts_offset`)
and for every tensor payload offset inside the trunk. Expert payload offsets
are not aligned individually — they are contiguous in directory order, which

**Reader cap `[CODE]` (implementation decision, 2026-10-26).** The reader
loads exactly three regions whole into RAM — the metadata K/V block, the
tensor directory and the expert directory (trunk payloads and expert
payloads are streamed, one `pread` each). A hostile multi-giB header could
therefore demand multi-giB allocations bounded only by the file size; the
reader caps each of those three regions at **256 MiB** and refuses larger
headers with a specific message. Real control regions stay far below the
cap (a 1 MiB expert directory already holds ~21,000 entries), so the cap
only rejects files no real model can be. It is checked at header
validation, before any region is read.
is the property that makes one expert one `pread`.

```
+----------------------------------------------+  0
| HEADER (128 bytes, fixed)                    |
+----------------------------------------------+  128
| METADATA K/V BLOCK                           |
+----------------------------------------------+
| (pad to alignment)                           |
+----------------------------------------------+
| TENSOR DIRECTORY (one 64-byte entry/tensor)  |
+----------------------------------------------+
| (pad to alignment)                           |
+----------------------------------------------+
| EXPERT DIRECTORY (one 48-byte entry/expert)  |
+----------------------------------------------+
| (pad to alignment)                           |
+----------------------------------------------+
| RESIDENT TRUNK PAYLOADS (router, embeddings, |
| tokenizer, norms, KV)                        |
+----------------------------------------------+
| EXPERT PAYLOADS, contiguous, in directory    |
| order                                         |
+----------------------------------------------+  EOF
```

### 4.1 Header (128 bytes)

| Offset | Size | Field | Type | Constraint |
|---|---|---|---|---|
| 0 | 8 | `magic` | `uint8[8]` | `"HYDRA2\0\0"` exactly |
| 8 | 2 | `version_major` | `uint16` | `= 2` |
| 10 | 2 | `version_minor` | `uint16` | `= 0` |
| 12 | 4 | `header_bytes` | `uint32` | `= 128` |
| 16 | 8 | `file_size` | `uint64` | `>= header_bytes` and `== ` the real size on disk |
| 24 | 8 | `metadata_offset` | `uint64` | `>= 128`, `< file_size` |
| 32 | 8 | `metadata_bytes` | `uint64` | `metadata_offset + metadata_bytes <= file_size` |
| 40 | 8 | `tensor_dir_offset` | `uint64` | aligned, `>= 128` |
| 48 | 8 | `tensor_dir_count` | `uint64` | `>= 1`, and `count * 64 <= file_size` |
| 56 | 8 | `tensor_dir_bytes` | `uint64` | `= tensor_dir_count * 64` |
| 64 | 8 | `expert_dir_offset` | `uint64` | aligned, `>= 128` |
| 72 | 8 | `expert_dir_count` | `uint64` | `>= 0`, `count * 48 <= file_size` |
| 80 | 8 | `expert_dir_bytes` | `uint64` | `= expert_dir_count * 48` |
| 88 | 8 | `trunk_offset` | `uint64` | aligned |
| 96 | 8 | `trunk_bytes` | `uint64` | `trunk_offset + trunk_bytes <= file_size` |
| 104 | 8 | `experts_offset` | `uint64` | aligned |
| 112 | 8 | `experts_bytes` | `uint64` | `experts_offset + experts_bytes <= file_size` |
| 120 | 4 | `header_crc32` | `uint32` | CRC-32 of bytes `[0, 120)` |
| 124 | 4 | `reserved` | `uint32` | `= 0` |

`file_size` exists so a truncated transfer is detectable without seeking to
the end. **[SPEC]**

### 4.2 Tensor directory entry (64 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 32 | `name[32]`, NUL-terminated ASCII |
| 32 | 2 | `role` (`0` trunk tensor, `1` router, `2` embedding, `3` norm, `4` KV, `5` shared expert, `6` **reserved**, `7` attention (`attn_q/k/v/o.{L}`), `8` vocabulary (`vocab` UTF-8 `0x0050` / `merges` `0x0051`), `9` LM head) |
| 34 | 2 | `type_id` (see §3.2) |
| 36 | 2 | `n_dims` (`1..4`) |
| 38 | 2 | `reserved` (`= 0`) |
| 40 | 8 | `offset` — **relative to `trunk_offset`**, `uint64` |
| 48 | 8 | `n_elements` — `uint64`, `>= 1` |
| 56 | 8 | `n_bytes` — `uint64`, `= n_elements * bytes_per_element(type_id)` |

Trunk tensors live inside the **resident** region. A reader maps
`[trunk_offset, trunk_offset + trunk_bytes)` and nothing else. **[SPEC]**

### 4.3 Expert directory entry (48 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `layer` | MoE layer index, `uint32` |
| 4 | 4 | `expert_id` | expert index **within that layer**, `uint32` |
| 8 | 8 | `offset` | **relative to `experts_offset`**, `uint64` |
| 16 | 8 | `length` | bytes, `uint64`, `>= 1` |
| 24 | 8 | `first_element` | index of this expert's first weight in the flattened parameter stream, `uint64` |
| 32 | 4 | `type_id` | see §3.2 |
| 36 | 4 | `crc32` | CRC-32 (IEEE, reflected, poly `0xEDB88320`) of exactly the `length` payload bytes |
| 40 | 8 | `reserved` | `= 0` |

**Ordering is normative.** Entries are sorted ascending by
`(layer, expert_id)`, and payload offsets are assigned in that same order, so
`expert[i].offset == sum(length[j] for j < i)`. A reader may therefore binary-
search the directory for a layer, and one expert read is one contiguous
`pread`. **[SPEC]**

### 4.4 Routing metadata (in the K/V block)

| Key | Type | Meaning |
|---|---|---|
| `v2.architecture` | string | must match §3.1 |
| `v2.layers` | `uint32` | number of transformer layers |
| `v2.moe_layers` | `uint32` | number of layers that route |
| `v2.top_k` | `uint32` | `>= 1`, `<= 64` |
| `v2.experts_per_layer` | `uint32` | `>= top_k` |
| `v2.router_dtype` | `uint32` | `type_id` of the router logits |
| `v2.expert_bytes_typ` | `uint64` | nominal bytes per expert, for budgeting only |
| `v2.router_runs_ahead` | `uint32` | `0` or `1`; whether one-layer-ahead prefetch is legal for this model |

`v2.top_k` is **required**, not defaulted. A reader that finds no `top_k` must
refuse the file: guessing it would change which experts are read, and a
silently different routing set is a wrong answer, not a slow one. **[SPEC]**

**Inference metadata (round 12, 2026-10-30).** The MoE module
(`src/hydra_moe.c`) consumes a `moe-transformer-v1` file and enforces these
additional keys; the container reader stays structural and does not check
them. Missing or malformed values are a refusal (code −4), never a default:

| Key | Type | Meaning |
|---|---|---|
| `v2.dim` | `uint32` | hidden size `> 0` |
| `v2.vocab_size` | `uint32` | vocabulary size `> 0` |
| `v2.n_heads` | `uint32` | query heads `> 0` |
| `v2.n_kv_heads` | `uint32` | KV heads, `1..n_heads` AND `n_heads % n_kv_heads == 0` (GQA block mapping, round 13) |
| `v2.head_dim` | `uint32` | `> 0`, and `dim == n_heads * head_dim` (checked in 64-bit arithmetic, round 13) |
| `v2.inter` | `uint32` | FFN width `> 0` |
| `v2.expert_layout` | `uint32` | `0` F32 flat, `1` ternary + block scales (§3.2.2) |
| `v2.rope_theta` | `float32` | RoPE base, default `10000.0` |
| `v2.kv_sink` | `uint32` | pinned KV entries, default `4`, `<= 0x10000` |
| `v2.kv_ring` | `uint32` | ring KV entries, default `128`, `<= 0x10000` |
| `v2.tied_embedding` | `uint32` | `1` logits use the embed matrix, `0` the `lm_head` tensor |
| `v2.bos_token` | `uint32` | beginning-of-sequence token, default `0` |

All ten are required for `moe-transformer-v1` files opened through the MoE
module; the required trunk tensors are `embed`, per layer
`attn_q/k/v/o.{L}`, `attn_norm.{L}`, `ffn_norm.{L}`, `router.{L}`,
`ffn_gate/up/down.{L}` (roles per §4.2), and `final_norm`, plus `lm_head`
when `v2.tied_embedding` is `0`. The shape relations are enforced at open
(refusal code −7) with 64-bit products — a 32-bit `n_heads * head_dim`
could wrap around and accidentally "equal" `dim` (round 13, Qodo #17).
Attention uses the llama.cpp GQA BLOCK mapping: query head `i` reads KV
head `i / (n_heads / n_kv_heads)` (contiguous groups share one KV head);
the pre-fix interleave `i % n_kv_heads` is wrong for any
`n_kv_heads > 1` (round 13, Qodo #26). A KV cache passed to
`hydra_moe_forward` whose vector length differs from
`n_kv_heads * head_dim` is refused (−1) — the head-strided reads would
otherwise wander outside the allocated slots (round 13, Qodo #28).
**[CODE]**

**K/V block wire format `[CODE]` (implementation decision, 2026-10-26).** The
specification names the keys but not the block layout; the reader and writer
implement:

```
u32 count
per pair: u32 key_len, u32 val_len, key bytes, value bytes
```

all little-endian, keys 1..63 printable ASCII and unique, values up to 65536
bytes. The reader checks the value width of every required key above (u32
keys must be exactly 4 bytes, `v2.expert_bytes_typ` exactly 8) and refuses
trailing bytes, duplicates, and non-ASCII keys. `[CODE]`
`src/hydra_v2_container.c` (`parse_metadata`),
`tools/make_model.py` (`build_v2`).

---

## 5. Validation rules

### 5.1 Both bounds, always

> **Every offset and every length is validated at BOTH ends.**
> `offset >= <region base>` **and** `offset + length <= region_end`.
>
> This is not advice. This repository's own history contains a real bug from
> validating only the upper bound, which is why it is written here as a rule
> with a named rationale. **(.agents/rules.md R14, R28)**

Concretely, a reader must reject the file unless **all** of the following hold:

1. `file_size >= header_bytes` and `file_size` equals the real size on disk.
2. Every directory `offset` is `>= ` its region base **and** `offset + n_bytes`
   is `<= ` that region's end, where the region end is derived from the header
   and independently from `file_size`. Both derivations must agree.
3. Every **count** satisfies `count * entry_size <= file_size` **and**
   `count <= ` a documented maximum. Multiplication is checked in 64-bit with
   an explicit overflow guard before the multiply, never after.
4. Every element count is exactly representable in its type: a `Q8_0` tensor
   with a non-multiple-of-32 element count is rejected (this check already
   exists in `tools/gguf_reader.py`, `Tensor.byte_size()`). **[CODE]**
5. `crc32` is verified for every expert **when it is read**, not at open time.
   Verifying all of them at open would defeat the purpose of streaming.
   **[SPEC]**
6. `header_crc32` is verified at open. A mismatch is a refusal, not a warning.

### 5.2 Refusals must be loud and specific

Every rejection names the field, the value, and the bound it violated, in the
user's terms — never an internal path, a stack trace, or a generic "invalid
file" (.agents/rules.md R27). A reader that cannot name the violation is a reader
whose validation is incomplete.

---

## 6. Worked budget: a 10 GB sparse model

**The numbers below are a worked example of the arithmetic, not a prediction
of this project on this hardware.** Every assumption is written out so the
reader can replace it.

### 6.1 Resident set

| Item | Bytes | Label |
|---|---|---|
| Router logits (layers x hidden) | 8 MiB | **[ESTIMATE]** hidden = 2048, fp16 |
| Token embeddings (32k x 2048, fp16) | 128 MiB | **[ESTIMATE]** |
| Output projection | 128 MiB | **[ESTIMATE]** |
| Attention norms + shared experts | 64 MiB | **[ESTIMATE]** |
| KV/state (context 4k) | 512 MiB | **[ESTIMATE]** 256 KiB per token |
| **Resident trunk total** | **≈ 840 MiB** | **[ESTIMATE]** |

### 6.2 Expert cache

| Item | Value | Label |
|---|---|---|
| Experts per layer | 64 | assumption |
| Bytes per expert | 12 MiB | assumption (int4-gs64 scale) |
| Cache budget | 512 MiB | declared application budget |
| Slots | `floor(512 MiB / 12 MiB)` = **42** | derived |
| Expert weight bytes resident | 504 MiB | derived |

**The budget also has to name what it does not control:** the kernel page
cache backing the file-backed expert mappings is **physical RAM on Android**,
charged against the app's footprint until the kernel reclaims it. A budget
that lists only the table above is not the memory the device sees. **[SPEC]**

Total accounted: `840 MiB (resident) + 504 MiB (slots) + 512 MiB (KV) +
work buffers`, leaving system headroom within a declared application budget.
**Measured RSS against that budget is a Phase D gate, and it has not been
measured.**

### 6.3 Throughput

The planning model, unchanged from the briefs:

```
tokens/s  <=  B_eff / bytes_read_per_token           (upper bound)
T_token   ~= T_router + max(T_disk_read, T_compute) + T_decode   (decomposition)
```

Worked with `bytes_read_per_token = top_k * bytes_per_expert`:

```
top_k = 8, bytes_per_expert = 12 MiB
  => 96 MiB per token
```

| Term | Value | Label |
|---|---|---|
| Sequential read, Snapdragon 8 Elite tablet (PCMark median, user submissions, Android 16) | 3,328 MB/s | **MEASURED elsewhere, on a different device** |
| Random read, same device, same run | 55 MB/s | **MEASURED elsewhere, on a different device** |
| Samsung UFS 4.0 sequential read | 4,200 MB/s | **SPEC — manufacturer peak datasheet** |
| **`B_eff` on the target tablet** | — | **NOT MEASURED. BLOCKED: no Snapdragon 8 Elite / Android 15 device is reachable.** |
| Upper bound at `B_eff = 3,328 MB/s` | 96 MiB / 3,328 MB/s ≈ **29 tok/s** | **ESTIMATE — and invalid**, because it assumes a sequential stream that sparse expert reads are not |
| Reading at the same device's *random*-read figure | ≈ **0.5 tok/s** | **ESTIMATE — and invalid**, because the block size of PCMark's random test is not stated and expert reads are large |

> **PLACEHOLDER — NOT MEASURED.** Neither row above is a result. They are
> included to show how far apart the answer is depending on an assumption
> nobody has measured, which is exactly why `B_eff` must be a measured input
> to the Phase D gate and never a design constant. For orientation, Colibri
> reports **12.7 GB per token** and **0.05–0.1 tok/s cold** on a 25 GB
> desktop **[MEASURED by Colibri]** — the same order of conclusion from a
> real run.

### 6.4 Reading strategy (Phase D/E design, not implemented)

- `pread` is the portable Android baseline. One expert is one contiguous read
  because §4.3 makes offsets normative.
- Expert slots are leased; a leased expert cannot be evicted (Phase D).
- `posix_fadvise(POSIX_FADV_DONTNEED)` is an **advisory hint only**. It is
  not a guarantee that the kernel reclaims anything, and no claim in this
  repository may depend on it.
- `io_uring` is optional and capability-checked at runtime; Android does not
  guarantee it. The fallback is `pread` and must remain correct when
  `io_uring` is unavailable.
- Prefetch and batch-union (Phase E) are instrumented before they are
  believed: predicted routing vs actual routing, bytes read, hit rate, and
  **wasted prefetch** are all reported separately.

---

## 7. Compatibility rules

1. **v1 files and v1 behaviour are unchanged.** No v1 header field, weight
   byte, or inference result changes.
2. **A v1 loader refuses a v2 file** on the magic, with the v2 magic named.
3. **A v2 loader refuses a v1 file** on the magic, with the v1 magic named.
   A v2 reader is not a v1 reader with extra features.
4. The v2 file extension is `.hydra2`. The v1 loader does not consider it.

## 8. Open questions for review (Gate B)

1. Is `first_element` in the expert directory needed by any planned consumer,
   or is it speculative weight? It is 8 bytes per expert — at 19,456 experts
   that is 156 KiB of directory. Keeping it only if a consumer exists.
2. Should `router_runs_ahead` be a file-level flag or a per-model property
   enforced at conversion time? Currently specified file-level.
3. What is the largest `top_k` worth supporting? `64` is a guess.
4. Is CRC-32 the right integrity primitive here, or is a per-expert length
   plus the contiguous ordering enough? CRC-32 costs a pass over every byte
   read, which is not free when read bandwidth is the bottleneck.