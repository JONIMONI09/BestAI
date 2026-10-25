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
4       2     version         uint16  = 1 (or 2, see "Version 2")
6       2     vocab_size      uint16  (1..1024)
8       4     dim             uint32  (1..64; version 2 requires dim % 4 == 0)
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
   to zero. Refused with error `-8`, not rounded (rules.md R11, R14).
2. `weights_len >= layers * dim / 2` — the v2 read size. The loader validates
   the ACTUAL read size per version: a v1 file keeps its exact
   `layers * dim` requirement, a v2 file is checked against `layers * dim / 2`
   (a minimal v2 file declaring exactly the bytes it uses is therefore valid).
3. The v2 file written by `test_version_two_format` is byte-for-byte
   equivalent to the v1 file it is compared against: identical aggregates and
   identical 40-token streams. That parity test is the proof the packing is
   the same model, not just a file the loader likes.

There is no shipped writer for version 2: `tools/make_model.py` and
`tools/gguf_to_hydra.py` both emit version 1, because the converter's exactness
guarantee (A/B written as literal layer counts) is expressed in v1's one-byte-
per-pair layout. Version 2 exists to read files produced elsewhere, and the
test suite produces its own fixtures for it.

## Reference Writer

`tools/make_model.py` is the canonical Python reference:

```python
header = struct.pack("<IHHIIII", MAGIC, VERSION, VOCAB, DIM, LAYERS,
                     weights_offset, num_packed_bytes)
```

## Validation Rules (Loader, `src/hydra_engine.c`)

1. `magic == 0x48594452`
2. `version == 1` or `version == 2` (version 2 additionally requires `dim % 4 == 0`, see "Version 2")
3. `1 <= dim <= 64` and `1 <= vocab_size <= 1024`
4. `layers <= 4096` (`HYDRA_MAX_LAYERS`) — bounds the worst-case accumulator magnitude to `4096 × 254`, well below `INT32_MAX`. (Discovered via proof-of-concept: without this rule, `layers = 16909321` with `dim = 1` and all weights `+1` overflows a signed accumulator after ~16.9 MB of file.)
5. `weights_offset >= 24` — the weight region may not overlap the header, otherwise header bytes would be executed as weights.
6. `weights_offset + weights_len <= file_size` — prevents out-of-bounds reads past the mmap. **The sum is computed in 64 bits**, even though both operands are `uint32`: in 32-bit arithmetic `0xFFFFF000 + 0x1000` wraps to `0`, the model looks valid, and the aggregation walk runs off the mapping. Measured: with a 32-bit sum, `tests/test_large_model.c` loses its child process with SIGSEGV.
7. `layers * dim <= weights_len` — the **loader** reads exactly `layers × dim` packed bytes once, when building the aggregates, so the declared weight region must cover them. The per-token step does not traverse the weight region at all. (Discovered via proof-of-concept: without this rule, a crafted header could read up to ~4 GiB past the file mapping.)

If any rule is violated, the mapping is torn down immediately through a single cleanup path and a negative error code is returned (`-5` magic, `-7` version, `-8` dim/vocab, `-9` file bounds, `-10` weight region, `-11` layer cap, `-12` header overlap).

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
