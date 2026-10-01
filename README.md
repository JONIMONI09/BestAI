# Hydra-Stone

**Zero-RAM, ternäre O(1)-Inferenz-Engine — wirf jedes `.hydra`-Modell hinein und los.**

```bash
./hydra-run mein_modell.hydra 123
```

Hydra-Stone ist eine C99-Engine, die Sprachmodell-Gewichte als **2-Bit-Ternärwerte** ({-1, 0, +1}) direkt aus der Datei über `mmap` streamt — ohne sie jemals in den Heap zu laden. Der Inferenzschritt besteht aus reinen Integer-Additionen, der RAM-Verbrauch ist **unabhängig von der Modellgröße konstant (O(1))**.

---

## Warum das schnell ist

| Ansatz | Multiplikationen/Token | RAM-Wachstum | Typischer Flaschenhals |
|---|---|---|---|
| FP32 Transformer | O(N² · d) | KV-Cache wächst unbeschränkt | HBM-Bandbreite |
| INT8-Quantisierung | O(N² · d) | KV-Cache wächst unbeschränkt | Dequantisierungs-Overhead |
| **Hydra-Stone (ternär + mmap)** | **0** (nur Addition) | **keines (O(1)-State)** | nur Disk/Cache-Bandbreite |

Drei Hebel:

1. **Ternäre Gewichte** — keine Multiplikation nötig: `y = Σ_{w=+1} x − Σ_{w=−1} x`
2. **mmap-Paging** — der Kernel lädt nur die 4-KiB-Seiten, die der Compute-Cursor tatsächlich berührt; bei Speicherdruck werden saubere Seiten sofort wieder freigegeben (kein Swap).
3. **O(1)-Rekurrenz-State** — statt einem wachsenden KV-Cache trägt ein fixer `int8_t[64]`-Vektor den Zustand.

## Schnellstart

```bash
# 1. Kompilieren
make

# 2. Test-Modell generieren
python3 tools/make_dummy_model.py test.hydra

# 3. Modell in die Engine werfen
./hydra-run test.hydra 123

# 4. Unit-Tests (18 Stück)
make test-run

# 5. Weboberfläche starten (PC-optimierte Konsole)
make ui    # → http://localhost:8787
```

## Weboberfläche (PC)

Die **Hydra-Stone Console** ist eine dunkle Terminal-Style-Oberfläche mit:

- **Modell-Panel** — dim/vocab/layers des geladenen Modells
- **Inferenz-Steuerung** — Start-Token & Steps, Enter startet
- **Live-Statistiken** — ms gesamt, ms/Token, Token/s
- **Token-Stream-Chart** — Canvas-Visualisierung der Output-Sequenz
- **O(1)-State-Heatmap** — 64 State-Zellen live eingefärbt
- **Koexistenz-Axiom-Slider** — H(s) von −0.5 bis 1.0, Blockiert/ Erlaubt in Echtzeit

Architektur: `server.js` (Node, **keine npm-Abhängigkeiten**) → `hydra-run --json` (C-Engine), Frontend reines HTML/CSS/JS unter `public/`.

| Endpunkt | Methode | Beschreibung |
|---|---|---|
| `/api/model` | GET | Header-Info des Modells |
| `/api/infer` | POST | `{token, steps}` → JSON mit Tokens, State, Timing |
| `/api/axiom?h=0.5` | GET | Axiom-Gate-Simulation |

## Features

- ✅ **Zero-Heap-Inferenz** — Gewichte werden nie kopiert, nur gemappt
- ✅ **Ternäre Linearmathematik** — 2 Bit/Gewicht, keine FP-Multiplikation im Inner Loop
- ✅ **Koexistenz-Axiom** — `humanity ≤ 0 ⇒ Utility = −∞`, hartes Safety-Gate vor jeder Aktion
- ✅ **NEON-SIMD-Kernel** — 16 parallele ternäre Akkumulationen auf ARM (auto-aktiv via `__ARM_NEON`)
- ✅ **Härtung** — Header-Validierung, OOB-Schutz, Sättigungsarithmetik, 18 Unit-Tests
- ✅ **Web-Console** — PC-UI mit Live-Visualisierung (`make ui`)
- ✅ **C99, keine Abhängigkeiten** — läuft auf 32-Bit ARMv7, x86-64, alles dazwischen

## Projektstruktur

```
├── include/hydra_model.h      Public API + Binärformat-Header
├── src/hydra_engine.c         mmap-Loader, ternäre Inferenz, Axiom-Gate
├── src/main.c                 CLI
├── src/hydra_neon.h           ARM-NEON-Kernel (optional, auto-erkannt)
├── tools/make_dummy_model.py  Testmodell-Generator (Format-Referenz)
├── tests/test_engine.c        18 Unit-Tests (inkl. OOB-PoC-Regression)
├── server.js                  UI-Server (Node, 0 npm-Dependencies)
├── public/                    Hydra-Stone Console (HTML/CSS/JS)
├── docs/ARCHITECTURE.md       Architektur & Mathematik
├── docs/FORMAT.md             .hydra-Binärformat-Spezifikation
└── LICENSE                    MIT
```

## Die Mathematik in 60 Sekunden

**Quantisierung (absmean):**
```
γ = 1 / mean(|W|)          W̃ = clip(round(γ · W), -1, +1)
```

**Forward-Schritt ohne Multiplikation:**
```
acc_i += (+1) · x_j   für jedes w_ij = +1
acc_i += (−1) · x_j   für jedes w_ij = −1
```

**Speicherbedarf pro Parameter: 2 Bit** (FP16: 16 Bit → **8× kleiner**, INT4: 4 Bit → **2× kleiner**).

| Parameter | FP16 | INT4 | Hydra 2-Bit |
|---|---|---|---|
| 0.5 B | 1000 MiB | 250 MiB | 125 MiB |
| 1.0 B | 2000 MiB | 500 MiB | 250 MiB |
| 1.5 B | 3000 MiB (OOM) | 750 MiB | 375 MiB |

## Roadmap

- [ ] Vollständiger Transformer-Forward (RMSNorm, RoPE, SwiGLU) über dem ternären Kern
- [ ] Min-P-Sampling & Repetition-Penalty
- [ ] GGUF/safetensors-Import mit automatischer absmean-Ternärisierung
- [ ] AVX2/AVX-512-LUT-Kernel für x86
- [ ] Streaming-Ring-Buffer-KV mit Attention-Sinks

Beiträge willkommen — siehe `docs/FORMAT.md` für die Binärspec, dann los.

## Lizenz

MIT — siehe [LICENSE](LICENSE).
