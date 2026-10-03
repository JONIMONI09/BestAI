# The `.hydra` Binary Format (v1)

## Layout

```
Offset  Size  Field           Type
──────  ────  ──────────────  ─────────────────
0       4     magic           uint32  = 0x48594452 ("HYDR", little-endian)
4       2     version         uint16  = 1
6       2     vocab_size      uint16  (1..1024)
8       4     dim             uint32  (1..64)
12      4     layers          uint32
16      4     weights_offset  uint32  (byte offset of the weights in the file)
20      4     weights_len     uint32  (byte length of the weights)
24      ...   weights         uint8[] (2 ternary weights per byte)
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

## Reference Writer

`tools/make_model.py` is the canonical Python reference:

```python
header = struct.pack("<IHHIIII", MAGIC, VERSION, VOCAB, DIM, LAYERS,
                     weights_offset, num_packed_bytes)
```

## Validation Rules (Loader, `src/hydra_engine.c`)

1. `magic == 0x48594452`
2. `version == 1`
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

Supporting a single model whose weights start at or above 4 GiB needs v2 (64-bit offsets) or
a split mapping with `mmap64`/`off64_t`. It does **not** need more RAM, which is the point:
the requirement "complete loading regardless of RAM" is already met on 64-bit platforms.

## Density, stated exactly (Honest Note)

The format stores **2 ternary weights per byte, i.e. 2 bits per weight on disk**: `w1` in bits 0–1, `w2` in bits 2–3, with the upper 4 bits reserved. One byte therefore covers one `(layer, dim)` pair, and `weights_len = layers × dim` **bytes** (verified: a 4-layer, dim-64 model declares `weights_len = 256`).

The reserved code `11` is decoded as `0`, so a byte carries `3 × 3 = 9` distinct states, i.e. `log₂9 ≈ 3.17` bits of information **per byte** — `log₂3 ≈ 1.58` bits **per weight**. The honest figures are therefore:

- **allocated:** 2 bits per weight,
- **informational:** ~1.58 bits per weight,
- **used:** half of each byte (4 of 8 bits).

Packing 4 weights per byte in v2 does not change the weight data density — it stays 2 bits per weight. What it removes is the wasted half of every byte, which halves the file size.

## Planned Extensions (v2)

- 64-bit offset fields, so weights may start at or above 4 GiB (see "Model Size vs RAM" —
  this is the only size limit that RAM does not solve)
- Per-layer scale factors (γ from absmean quantization)
- 4-weights-per-byte packing (same 2 bits/weight, but no wasted byte — roughly halves file size)
- Checksum (xxHash) over the weight region
- **Aggregated container** holding only `A[i] = Σ_l w1[l][i]` and `B[i] = Σ_l w2[l][i]`, signalled by a `HYDRA_AGGREGATE_V2` flag. The layer loop in v1 is algebraically redundant (see "Known Structural Redundancy" in `docs/ARCHITECTURE.md`), so v1 models compress losslessly into this form. v1 files keep loading unchanged.
