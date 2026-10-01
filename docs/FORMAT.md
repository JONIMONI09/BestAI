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

The header is **exactly 24 bytes** (`#pragma pack(1)`, `struct pack "<IHHIIII>"` in Python).

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

`tools/make_dummy_model.py` is the canonical Python reference:

```python
header = struct.pack("<IHHIIII", MAGIC, VERSION, VOCAB, DIM, LAYERS,
                     weights_offset, num_packed_bytes)
```

## Validation Rules (Loader, `src/hydra_engine.c`)

1. `magic == 0x48594452`
2. `version == 1`
3. `1 <= dim <= 64` and `1 <= vocab_size <= 1024`
4. `weights_offset + weights_len <= file_size` — prevents out-of-bounds reads past the mmap.
5. `layers * dim <= weights_len` — the inference step reads exactly `layers × dim` packed bytes, so the declared weight region must cover them. (Discovered via proof-of-concept: without this rule, a crafted header could read up to ~4 GiB past the file mapping.)

If any rule is violated, the mapping is torn down immediately and a negative error code is returned.

## Known Density Limitation (Honest Note)

The format stores **2 weights per byte (4 bits/weight effective)** while the README's comparison table advertises the 2-bit/weight density that 4-weights-per-byte packing would achieve. The upper 4 bits are reserved. Moving to 4 weights per byte is planned for v2 and would halve file sizes; until then, the advertised "2 bits" figures describe the *target* density, not the current on-disk density.

## Planned Extensions (v2)

- 64-bit fields for models > 4 GiB
- Per-layer scale factors (γ from absmean quantization)
- 4-weights-per-byte packing (true 2 bits/weight)
- Checksum (xxHash) over the weight region
