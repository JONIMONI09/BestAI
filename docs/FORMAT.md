# Das `.hydra`-Binärformat (v1)

## Layout

```
Offset  Größe  Feld            Typ
──────  ─────  ──────────────  ─────────────────
0       4      magic           uint32  = 0x48594452 ("HYDR", little-endian)
4       2      version         uint16  = 1
6       2      vocab_size      uint16  (1..1024)
8       4      dim             uint32  (1..64)
12      4      layers          uint32
16      4      weights_offset  uint32  (Byte-Offset der Gewichte im File)
20      4      weights_len     uint32  (Byte-Länge der Gewichte)
24      ...    weights         uint8[] (2 ternäre Gewichte pro Byte)
```

Der Header ist **exakt 24 Bytes** (`#pragma pack(1)`, struct pack `<IHHIIII>` in Python).

## Gewichtspacking

Ein Byte trägt **zwei** ternäre Gewichte in den unteren 4 Bits:

```
Bit:    7 6 5 4 3 2 1 0
        ^^^^^^^^^ ^^^^^
        reserved   w1 (untere 2 Bits)
                  w2 (Bits 2-3)
```

| Code | Wert |
|---|---|
| `00` | 0 |
| `01` | +1 |
| `10` | −1 |
| `11` | reserved (Loader behandelt es als 0) |

Die Iterationsreihenfolge ist **row-major über Layer × dim**: erst alle `dim` Paare von Layer 0, dann Layer 1, usw. Innerhalb eines Layer-Durchlaufs wird pro Byte zuerst `w1`, dann `w2` konsumiert.

## Referenz-Writer

`tools/make_dummy_model.py` ist die kanonische Python-Referenz:

```python
header = struct.pack("<IHHIIII", MAGIC, VERSION, VOCAB, DIM, LAYERS,
                     weights_offset, num_packed_bytes)
```

## Validierungsregeln (Loader, `src/hydra_engine.c`)

1. `magic == 0x48594452`
2. `version == 1`
3. `1 <= dim <= 64` und `1 <= vocab_size <= 1024`
4. `weights_offset + weights_len <= file_size` — verhindert Out-of-Bounds-Reads hinter dem mmap.

Bei Verletzung einer Regel wird das Mapping sofort wieder aufgehoben und ein negativer Fehlercode zurückgegeben.

## Erweiterungen (geplant, v2)

- 64-Bit-Felder für Modelle > 4 GiB
- Per-Layer-Skalierungsfaktoren (`γ` aus absmean-Quantisierung)
- Checksumme (xxHash) über die Gewichtsmasse
