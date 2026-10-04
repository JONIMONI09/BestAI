# The `.hydra2` Container Format (v2) — **specification only**

> **Status: SPECIFICATION. There is no v2 loader in this repository.**
> `src/hydra_v2_reader.c` does not exist and no v2 file can be opened. This
> document exists so the format can be reviewed and agreed **before** any
> code is written against it.
>
> **v1 is untouched.** Every `.hydra` v1 file written by any v1 release loads
> under any v1 loader, and no v2 change alters that. v1 and v2 have different
> magics, different version fields and different file extensions
> (`.hydra` vs `.hydra2`). A v1 loader must refuse a v2 file loudly, not read
> it partially.

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
[`docs/HYDRA2-RESEARCH.md`](HYDRA2-RESEARCH.md).

**So:** the two passages above are kept, and clearly marked, rather than deleted
— deleting a documented observation because it lost an argument is how a
repository stops being able to explain why a design was rejected. Anyone
implementing "v2" implements **this** document. The aggregated container stays
available as a possible *v1.1 file-level optimisation* if it is ever wanted, and
that is the only sense in which it is still on the table.

### Implementation status of this document: nothing

Stated plainly, because the tensor table in §3.2 used to imply otherwise:

- **No part of the `.hydra2` container is implemented.** There is no reader, no
  writer, no magic, no header parser, no expert directory, no routing metadata,
  no expert cache. Not one byte of a `.hydra2` file can be produced or consumed
  by this repository.
- The **Q8_0 decoder** that appears in §3.2 is real code, but it belongs to the
  **GGUF import path** (`tools/gguf_reader.py`, feeding
  `tools/gguf_to_hydra.py`). It is a Python reader for somebody else's file
  format that converts to a **v1** `.hydra`. It is not a v2 reader and does not
  become one by being listed here.
- The **research** that motivates the sparse-MoE path is in
  [`docs/HYDRA2-RESEARCH.md`](HYDRA2-RESEARCH.md). Its benchmark plan is
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

**None of these is implemented in a v2 reader — there is no v2 reader.** The
first five rows exist as decoders somewhere in this repository only in the sense
that `tools/gguf_reader.py` can read F32/F16/BF16/Q8_0 for the **GGUF import
path**, which produces a **v1** `.hydra`. The last three are listed so that a
future reader rejects them **by name** rather than by falling through to
"unsupported". **[SPEC]**

### 3.3 Out of scope

Mamba / SSM / structured-state-space layers are **out of scope for v2**. They
are a separate research branch requiring their own evidence and approval.
**[SPEC]**

---

## 4. Container layout

All integers are **little-endian**, without exception. A big-endian host must
read through explicit byte-wise readers, exactly as v1 does
(`rd_u32le` in `src/hydra_engine.c`). **[CODE]**

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
| 32 | 2 | `role` (`0` trunk tensor, `1` router, `2` embedding, `3` norm, `4` KV, `5` shared expert) |
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

---

## 5. Validation rules

### 5.1 Both bounds, always

> **Every offset and every length is validated at BOTH ends.**
> `offset >= <region base>` **and** `offset + length <= region_end`.
>
> This is not advice. This repository's own history contains a real bug from
> validating only the upper bound, which is why it is written here as a rule
> with a named rationale. **(rules.md R14, R28)**

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
file" (rules.md R27). A reader that cannot name the violation is a reader
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