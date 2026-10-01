# Hydra-Stone Architektur

## Überblick

```
┌────────────────────────────────────────────────────────────┐
│                      hydra-run (CLI)                       │
├────────────────────────────────────────────────────────────┤
│  hydra_engine_load()  →  mmap() + Header-Validierung       │
│  hydra_engine_step()  →  ternäre O(1)-Inferenz             │
│  hydra_verify_axiom() →  Koexistenz-Axiom (Safety-Gate)    │
├────────────────────────────────────────────────────────────┤
│                 .hydra-Modellformat (v1)                   │
│  [24B Header][2-Bit ternäre Gewichte, gepackt]             │
└────────────────────────────────────────────────────────────┘
```

## Komponenten

| Datei | Aufgabe |
|---|---|
| `include/hydra_model.h` | Public API, Header-Struct, Defines |
| `src/hydra_engine.c` | Loader (mmap), Inferenz-Schritt, Axiom-Gate |
| `src/main.c` | CLI-Einstiegspunkt |
| `src/hydra_neon.h` | NEON SIMD-Kernel — **aktiv integriert**: `step()` nutzt ihn auf ARM in 16er-Chunks; bit-identischer Skalar-Fallback für Reste und x86 |
| `tools/make_dummy_model.py` | Testmodell-Generator (Reference-Implementierung des Formats) |
| `tests/test_engine.c` | 16 Unit-Tests |

## Speichermodell: Zero-RAM via mmap

1. Gewichte werden **nie** in den Heap kopiert.
2. `mmap(PROT_READ, MAP_SHARED)` mappt die Datei in den Adressraum.
3. `madvise(MADV_SEQUENTIAL)` teilt dem Kernel das Zugriffsmuster mit → aggressives Read-Ahead.
4. Seiten (4 KiB) werden vom Kernel on-demand geladen und bei Speicherdruck sofort wieder freigegeben (clean pages → kein Swap nötig).
5. Der Laufzeit-Zustand ist ein **fixer** `int8_t[64]`-Vektor → O(1)-RAM.

**Konsequenz:** Die inferenzseitige RAM-Nutzung ist unabhängig von der Modellgröße konstant. Ein 1-GB-Modell verbraucht keinen Heap — nur den Page Cache, den der Kernel nach Bedarf evicted.

## Ternäre Mathematik

Gewichte: `W ∈ {-1, 0, +1}`, 2 Bit pro Gewicht, 4 Gewichte pro Byte (hier: 2 pro Byte genutzt, 2 Bit reserved).

```
00 = 0    01 = +1    10 = -1    11 = reserved (als 0 behandelt)
```

Der GEMM-Kern reduziert sich auf Addition/Subtraktion:

```
y_i = Σ_j w_ij · x_j   →   y_i = Σ_{w=+1} x_j  −  Σ_{w=−1} x_j
```

Keine FP32/FP16-Multiplikation im Inner Loop → maximaler ALU-Durchsatz auch ohne FPU (ARMv7-A VFP-frei lauffähig).

## SIMD-Strategie (ehrliche Bestandsaufnahme)

| Plattform | Pfad in `hydra_engine_step()` | Status |
|---|---|---|
| ARM64/ARMv7-A mit NEON | `hydra_neon_accumulate_chunk()` — 16 Lanes pro Chunk | **aktiv**, von Seed-Roundtrip-Tests auf macOS-ARM64-CI abgedeckt |
| ARM, `dim % 16 != 0` | NEON für Vielfache von 16, Skalar für den Rest | aktiv |
| x86 / x86-64 | rein **skalar** | kein SIMD — AVX2 auf der Roadmap |

Beide Pfade sind **bit-identisch** (gleiche Decodier- und Akkumulationssemantik). Overflow-Analyse NEON: \|w\|≤1, \|token\|≤127, \|state\|≤127 → Produkte ≤ 16129, sicher in int16; Akkumulation in int32.

**Performance-Implication:** Auf x86 wird die ternäre Arithmetik aktuell nicht SIMD-beschleunigt — der Geschwindigkeitsvorteil dort kommt ausschließlich aus dem 2-Bit-Speicherformat (weniger Speicherbandbreite, mehr Cache-Hits) und der fehlenden FP-Multiplikation. Genuve SIMD-Beschleunigung auf x86 erfordert den AVX2-Kernel (Roadmap).

## Koexistenz-Axiom

```
U(s,a) = R(s,a) · I{H(s)=1}  −  ∞ · I{H(s)<1}
```

`hydra_verify_axiom()` implementiert das Gate: `humanity <= 0` → Utility auf `-1e9` gedrückt, Rückgabecode 0 (blockiert). Jeder Agent-Action-Pfad muss dieses Gate passieren, bevor externe Effekte ausgelöst werden.

## Sicherheits-Design

- Header-Validierung: Magic, Version, dim/vocab-Grenzen, `weights_offset + weights_len ≤ file_size` (verhindert Out-of-Bounds-Reads hinter dem mmap).
- State-Update mit Modulo + Sättigung → kein Signed-Overflow-UB.
- Testabdeckung: Garbage-Input, OOB-Header, NULL-Pointer, Determinismus.
