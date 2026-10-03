# Hydra-Stone

**Zero-RAM, ternary O(1) inference engine — drop in any `.hydra` model and go.**

```bash
./hydra-run my_model.hydra 123
```

A whole token sequence can be fed in as a prompt — every token before the last one is run through the engine as context, the last one is the seed for the generated continuation:

```bash
./hydra-run my_model.hydra 0 16 --prompt 7,9,11 --json
```

Hydra-Stone is a C99 engine that streams ternary weights ({-1, 0, +1}) directly from disk via `mmap`. The inference step consists purely of integer additions.

**Memory claim, stated honestly:** the engine performs **no heap allocation at all** — the only process memory it uses is the O(1) `int8_t[64]` state vector. The weight pages themselves are owned by the kernel page cache and are reclaimed under memory pressure by LRU eviction. The engine cannot and does not keep the model's resident set size at zero; build with `-DHYDRA_DROP_CACHE` to explicitly hand touched pages back after every step.

---

## Why It's Fast

| Approach | Multiplications/Token | RAM Growth | Typical Bottleneck |
|---|---|---|---|
| FP32 Transformer | O(N² · d) | KV cache grows unbounded | HBM bandwidth |
| INT8 quantization | O(N² · d) | KV cache grows unbounded | Dequantization overhead |
| **Hydra-Stone (ternary + mmap)** | **0** (addition only) | **none (O(1) state)** | disk/cache bandwidth only |

Three levers:

1. **Ternary weights** — no multiplication needed: `y = Σ_{w=+1} x − Σ_{w=−1} x`
2. **mmap paging** — the kernel faults in only the 4-KiB pages the compute cursor actually touches; clean pages are reclaimable under memory pressure (no swap traffic).
3. **O(1) recurrent state** — instead of a growing KV cache, a fixed `int8_t[64]` vector carries the state.

### A bug worth reading about

Until 2026-10-02 the NEON kernel decoded the ternary code `01` as **−1** instead of +1: `vceqq_u8` returns `0xFF` on a match, and `vsubq_s8(p, n)` therefore computed `0xFF − 0x00 = −1`. ARM produced **exactly negated weights** and silently different token sequences than x86, while the documentation claimed both paths were bit-identical.

It survived because *no test compared the two paths to each other* — every test compared the engine against fixed expectations, and each platform only ever ran its own path. The fix (`src/hydra_neon.h`, explicit `0x01`/`0xFF` mask normalisation) came together with `test_neon_matches_scalar`, which now runs a scalar reference **inside the same ARM build** and demands bit-identical tokens *and* state vectors. That test immediately caught a second latent defect in the SIMD store offsets, which would otherwise have shipped.

## Quick Start

```bash
# 1. Compile
make

# 2. Generate a starter model
python3 tools/make_model.py test.hydra

# 3. Feed the model to the engine
./hydra-run test.hydra 123

# 4. Unit tests (71 on x86-64, 77 on ARM with NEON)
make test-run

# 5. Start the web console (desktop-optimized)
make ui    # → http://localhost:8787
```

## Web Console (Desktop)

The **Hydra-Stone Console** is a dark terminal-style desktop UI featuring:

- **Model panel** — dim/vocab/layers of the loaded model
- **Inference controls** — start token & steps; Enter triggers a run
- **Live statistics** — total ms, ms/token, tokens/s
- **Token stream chart** — canvas visualization of the output sequence
- **O(1) state heatmap** — 64 state cells, colored live
- **Coexistence axiom slider** — H(s) from −0.5 to 1.0, Blocked/Allowed in real time

Architecture: `server.js` (Node, **zero npm dependencies**) → `hydra-run --json` (C engine); frontend is plain HTML/CSS/JS under `public/`.

| Endpoint | Method | Description |
|---|---|---|
| `/api/model` | GET | Header info of the model |
| `/api/models` | GET | Every `.hydra` under `models/`, with a real header; drives the chat model selector |
| `/api/models/inspect?path=` | GET | Full header **plus every rule the file violates**, each with a readable reason |
| `/api/models/upload?name=` | POST | Store a `.hydra` (`HYDRA_MAX_UPLOAD`, 64 MiB default) |
| `/api/models/import?name=` | POST | Convert a **`.gguf`** to `.hydra` and store both |
| `/api/infer` | POST | `{token, steps}` → JSON with tokens, state, timing |
| `/api/cancel` | POST | Abort the running inference (409 `cancelled`) |
| `/api/axiom?h=0.5` | GET | Axiom gate simulation |

## Features

- ✅ **Zero-heap inference** — weights are never copied, only mapped; no `malloc` in the inference path
- ✅ **Ternary linear math** — ternary weights, no FP multiplication in the inner loop. *Density note:* the v1 format stores 2 weights per byte, i.e. **4 bits per weight on disk** (true 2-bit packing via 4 weights/byte is planned for v2) — see `docs/FORMAT.md`.
- ✅ **GGUF import** — a `.gguf` is detected and ternarised into a real `.hydra` with absmean scaling, streaming so a multi-GB file does not have to fit in RAM. Python stdlib only: no numpy, no torch. See [`docs/GGUF-IMPORT.md`](docs/GGUF-IMPORT.md).
- ✅ **Coexistence axiom** — `humanity ≤ 0 ∨ NaN ∨ ±∞ ⇒ Utility = −∞`, hard safety gate before any action
- ✅ **NEON SIMD kernel** — 16 parallel ternary accumulations, **actively integrated** into `hydra_engine_step()` on ARM (`__ARM_NEON`); bit-identical scalar fallback on x86
  - *Honesty note:* the NEON path is active on ARM builds only. It is **not** merely covered indirectly — `tests/test_engine.c` runs a scalar reference implementation next to the NEON kernel **in the same ARM build** and requires bit-identical token sequences *and* state vectors. The x86 path is purely scalar; AVX2 is on the roadmap.
- ✅ **Hardening** — header validation (little-endian decode, layer cap, offset checks), OOB protection, overflow-free accumulation, 71 unit tests (77 on ARM with NEON)
- ✅ **Web console** — desktop UI with live visualization (`make ui`)
- ✅ **Android** — NDK/JNI build of the same C source; launches, loads its bundled model and completes inference on an emulator (x86-64, API 24). *Not yet verified on physical ARM hardware — no device was available.*
- ✅ **C99, zero dependencies** — runs on 32-bit ARMv7, x86-64, and everything in between

## Releases & the Android APK

Every `v*.*.*` tag produces a GitHub Release containing a **directly installable APK**:

```bash
# Install on a phone / emulator (Android 7.0 / API 24+)
adb install -r hydra-stone-<version>-android-arm64v8a-armeabiv7a-x86_64.apk
```

The APK bundles the same C engine (`libhydra.so` for arm64-v8a, armeabi-v7a and x86_64 — NEON active on ARM), the packed starter model (`assets/starter.hydra`, 280 bytes), and a launcher icon. The app **runs by itself** on launch — it copies the starter model into private storage and starts an inference run; untick *Run automatically on start* to skip that. Tapping **Run inference** executes the engine again and streams the tokens live.

**Importing your own model.** *Import model…* opens the system file picker (Storage Access Framework, `*/*` because `.hydra` has no reliable MIME mapping). The chosen file is streamed into the app's private storage, validated against the same header rules the C loader enforces, and only then renamed into place and used — the starter model stays as the fallback when nothing valid has been imported. Tokens are delivered from the JNI layer in blocks of 16 instead of one call per token; the per-token JNI transition cost more than the engine step itself at this model size.

The whole screen scrolls. A 320x640 device cannot fit the controls and the token log at once, and a root layout that does not scroll pushes the log below the visible area — the engine then runs while showing nothing, which is indistinguishable from "it does not start". *The starter model is never demo content: it is the shipped default, and any valid import replaces it.*

Release artifacts:

| File | Platform |
|---|---|
| `hydra-stone-*-android-*.apk` | Android, all three ABIs |
| `hydra-stone-debug.apk` | Android debug build, same code |
| `hydra-run-linux-x64` | Linux x86-64, statically linked |
| `hydra-run-linux-arm64` | Linux ARM64, statically linked |
| `hydra-run-macos-arm64` | macOS Apple Silicon (NEON active) |
| `SHA256SUMS.txt` | integrity check for every file |

**Release gates.** The release job refuses to publish unless every one of these passes: the x64 and arm64 CLI binaries produce **bit-identical token and state vectors**; the APK is signature-verified and contains `libhydra.so` for all three ABIs plus the model asset; Android Lint reports no findings.

**Signing.** Without repository secrets the APK is signed with the debug key — fully installable by sideloading, but not Play-Store-ready. Supplying `HYDRA_KEYSTORE_BASE64`, `HYDRA_KEYSTORE_PASSWORD`, `HYDRA_KEY_ALIAS` and `HYDRA_KEY_PASSWORD` switches the build to a real release key, and the release body states which one was used.

**Every push builds the APK.** A dedicated `android-apk` job in `ci.yml` runs on every push and pull request: it builds `assembleDebug` and `assembleRelease`, verifies the signature with `apksigner`, checks that `libhydra.so` is packaged for arm64-v8a, armeabi-v7a and x86_64 together with the model asset, and uploads the APK as a workflow artifact. A release is published only for a `v*.*.*` tag — but a broken app build can therefore never reach release day unnoticed. The JNI bridge is checked separately by `jni-signatures`: `javac -h` derives the JNI header from the Kotlin declarations and `tools/jni_signature_check.sh` proves every native method exists in the C bridge with a matching parameter list.

**Manual runs.** The release workflow can also be started from the Actions tab (*Run workflow*) without pushing a tag. Leave **tag** empty to get a build-only dry run: all jobs run — CLI binaries, APK build, signature verification, Android Lint, x64/arm64 parity — but **no release is published**. Enter a tag and tick **publish** to behave exactly like a tag push. The trigger and version logic are regression-tested by `tools/ci_release_version_test.sh` (job `release-config`), which replays the script straight out of the workflow file for five input cases.

**Automatic releases on every push to `main`.** The same workflow also runs on `push: branches: [main]`. It then decides on its own, from the remote tags: no tag yet → create `v1.0.0`; tag pointing at an older commit → patch bump; tag already on this commit → reuse it. The tag is created through the job credential, which does not re-trigger the tag trigger, so the build runs exactly once. Publishing is an explicit upsert — `gh release view` decides between `gh release edit` + `upload --clobber` and `gh release create` — and a final step asserts that all five expected assets are attached, so a release can never be "successful" while missing the APK. The version resolution is regression-tested for ten input cases, including a negative control against a deliberately broken copy of the workflow.

**APK signing.** No `HYDRA_KEYSTORE*` secret is configured in this repository, so the release APK is signed with the **debug key**: fully sideloadable, not Play-Store-ready. The release body says exactly that instead of claiming a signed release. Store `HYDRA_KEYSTORE_BASE64`, `HYDRA_KEYSTORE_PASSWORD`, `HYDRA_KEY_ALIAS` and `HYDRA_KEY_PASSWORD` as repository secrets to get a real release signature; the workflow decodes them only when the value is non-blank (an unconfigured secret arrives as an empty string, which once broke the first real release run).

## Web console

`make ui` (or `node server.js`) serves a dependency-free console on `PORT` (default 8787).

**Binding.** The console binds to `127.0.0.1` only. It exposes an engine that loads files from disk, so listening on every interface has to be an explicit decision:

```bash
HYDRA_ALLOW_REMOTE=1 node server.js   # then also reachable from the LAN
```

| Area | What it does |
|---|---|
| **Chat** | Type text, get a real reply from the compiled C engine. Words are mapped to token IDs through the stored vocabulary; a word without an ID is reported as unknown instead of being guessed. Replies can be shown as words or raw token IDs. **The whole prompt is fed to the engine**, not just its first token — `hydra_engine_prefill()` runs every prompt token through the same core step before generation starts. |
| **Models** | Every `*.hydra` file under `models/` is listed with its real header (dim, vocab, layers, size, validity) and can be selected. Trained and uploaded models are marked. |
| **Upload** | **Upload model** in the top bar stores a `.hydra` into `models/uploaded/` (`POST /api/models/upload?name=…`). The bytes are streamed to disk (cap: `HYDRA_MAX_UPLOAD`, 64 MiB by default, `2GB`/`512MiB`/plain bytes all accepted), then validated against the same rules the C loader enforces — magic, version, dim/vocab/layers bounds, `weights_offset`/`weights_len` inside the file, `layers × dim` covered by `weights_len`. An invalid file is rejected with the reason and never reaches a model path. Over the cap the server answers **413 with a body** (never a dropped connection) that names the limit and how to raise it. |
| **GGUF import** | **Import .gguf** converts a llama.cpp/Ollama checkpoint into `.hydra` (`POST /api/models/import?name=…`) with absmean scaling, and returns a conversion report: which source tensor fed each plane, its shape, rows read vs. rows sampled, the **absmean scale `gamma` and `mean|w|`**, how many weights ternarised to zero, the resulting density, the rescale factor, the resulting peak and the vocabulary size — plus the reason if it refused. The converted model appears in the chat model selector marked as converted, and the source vocabulary is carried over so chat words map to real token ids. |
| **Diagnostics** | `GET /api/models/inspect?path=models/starter.hydra` returns the header values and one entry per rule (`id`, `ok`, `message`) plus a `violations` list. It is the same analysis the upload route uses, so both quote the same reason. Known formats are **named**: a GGUF answers *"this is a GGUF model (llama.cpp / Ollama) — the engine reads .hydra files"* instead of a bare `wrong magic`. The report also states the ceiling of the current format (`limits.maxWeightsBytesForShape`), because "valid" and "can be 20 GB" are two different questions. The console shows the violated rules under any model marked *invalid header*. |
| **Engine output** | Token chart with metrics; the state vector and the raw log are collapsed by default. |
| **Training** | Paste a corpus (`3 3 3 3 -> 7 7 7 7`, or `1 2 3` for next-token training), pick vocab/dim/layers/epochs and train. Training writes a real `.hydra` file and is **verified against the compiled engine** before it is reported as successful; accuracy before/after is measured, never estimated. |
| **Settings** | Start token, steps, coexistence-axiom factor, per-model vocabulary editor. The drawer traps focus, closes on Escape and returns focus to its toggle. |

Only one inference runs at a time: **Send** and **Train** are disabled while a request is in flight and a **Cancel** button aborts the request. Cancel is real, not cosmetic — the server kills the engine process it started (`SIGTERM`, then `SIGKILL` after a grace period) and answers **409 `cancelled`**. Because each request owns its own child process, killing it cannot disturb another request. A client that simply disconnects is treated the same way.

Everything shown comes from the engine. There is no simulated output anywhere in the console.

**How the trainer works.** The engine's update is `acc[i] = Σ (w1·token + w2·state[i])` with the next token decoded from `acc[0]`. Only column 0 influences the decoder, so the trainer searches ternary moves there (`0/±1`), accepts a move only when replaying the **complete** sample afterwards yields more correct steps, and returns the best snapshot over all epochs. Accuracy therefore never drops below the seed weights. The engine has a small capacity by design — the console reports the real number instead of hiding it.

## Performance

The inference step is now two multiply-adds per dimension, not two per
(layer x dimension):

```
python3 tools/make_model.py models/starter.hydra
./hydra-run models/starter.hydra 0 --bench 20000
{"mode":"bench","engine":"cpu","model":"models/starter.hydra","dim":64,"layers":4,
 "vocab":512,"steps":20000,"warmup_layers":4,"ns_per_token_full":140.60,
 "ns_per_token_fast":15.75,"speedup":8.929,"tokens_equal":true,"load_ms":0.037,
 "weights_bytes":256,"rss_before_kb":948,"rss_after_kb":948,"rss_delta_kb":0,
 "note":"JNI callback costs are NOT included here; measure them with HydraBridge.benchmark() on the device"}
```

`load_ms` and `rss_delta_kb` are measured, not asserted: the loader reports how long the map took and how much resident memory the run added. For this model `rss_delta_kb` is **0**, because the 256 weight bytes are mmap'd and never copied — the kernel owns those pages.

* **Layer aggregation** — `A[i] = sum_l w1[l][i]` and `B[i] = sum_l w2[l][i]`
  are computed once at load time, because the token and the state vector
  are constant across layers. The layer loop left the hot path.
* **`--fast` (token-only)** — only dimension 0 is computed. After
  aggregation the dimensions are independent, so the token stream is
  **bit-identical** while the arithmetic drops to a single multiply-add.
  The remaining state entries are intentionally left stale.

Both are covered by equivalence tests against a reference implementation
that lives in the test file, plus negative controls. On the Android app the
**Benchmark** button runs the same three-path measurement across JNI
(`HydraBridge.benchmark()`). Whether a GPU would beat any of this is
analysed — with a measurement — in [`docs/gpu-feasibility.md`](docs/gpu-feasibility.md).

## Error reporting

Every layer has a crash handler, and every report can be copied as plain
text:

| Layer | What is caught | Where it shows up |
|---|---|---|
| Web console | `window.onerror`, unhandled promise rejections, failed engine/training requests | a dialog with a **Copy details** button, plus an "Errors" button in the top bar that lists everything recorded this session |
| Console server | uncaught exceptions, unhandled rejections, failed requests | `GET /api/errors`, polled by the console every second and shown in the same dialog |
| Android app | every uncaught exception on every thread | `last-crash.txt` in private storage, and `CrashActivity` with a **Copy report** button |

The clipboard path falls back from the async Clipboard API to a hidden
textarea, because the console is often opened over plain HTTP on a LAN
address where `navigator.clipboard` does not exist.

## Quality Gates

Every push and pull request runs the following. Each gate was verified locally **and** proven to fail on a deliberately reverted fix before being adopted — a linter that cannot fail is worthless.

| Surface | Gate | Catches |
|---|---|---|
| C | `gcc -fsyntax-only -Werror`, `clang -fsyntax-only -Werror` | strict C99 violations, two compiler frontends |
| C | GCC `-fanalyzer` | NULL derefs, leaks, use-after-free paths |
| C | Clang Static Analyzer | interprocedural path analysis |
| C | clang-tidy (`bugprone`, `cert`, `concurrency`, `clang-analyzer`) | API misuse patterns |
| C | cppcheck | dead code, resource misuse |
| C | flawfinder (level ≥ 4) | known-dangerous C functions |
| C | **ASan + UBSan** on the full test suite | out-of-bounds, signed overflow, UB — *this is the gate that flagged the layer-overflow bug as `signed integer overflow: 2147483640 + 127`* |
| NEON | AArch64 build under qemu + scalar/NEON parity check | platform-divergent SIMD |
| Web | ESLint 10 (`server.js`, `public/app.js`) | undefined vars, `eval`, sloppy globals |
| Android | Android Lint (must report **no issues**) | missing icon, hardcoded strings, API misuse, orientation locks |
| Android | APK permission gate (`aapt dump permissions`) | the app opens files through SAF, which needs **no** permission — any dangerous permission must be a decision, not a surprise |
| Android | Android autostart gate (`tools/android_autostart_check.sh`) | the app must actually run on its own: starter model in assets, wired to the load path, no `demo.hydra` reference left behind, log mirrored to logcat |
| Android | Android header-diagnostics gate (`tools/android_header_check.sh`) | the app and the server must apply the **same** header rules and quote the **same** reason |
| Node | `node --test tools/server_test.js` | upload cap, diagnostics, GGUF import, cancel — each with a negative control |
| Python | `python3 tools/gguf_test.py` | a synthetic GGUF goes in, a **real C engine run** comes out — the converter is checked against the engine, not against itself |
| Android | NDK clang build (stricter than host gcc) | JNI/Android header issues |
| Security | CodeQL (`c-cpp`, `javascript-typescript`, `security-extended`) | taint flows from HTTP input into `open()`/`execFile` |
| Security | Semgrep | pattern-based security rules |

Local reproduction is documented in the `/engine-ci-verify` skill.

## Project Structure

```
├── include/hydra_model.h      Public API + binary format header
├── src/hydra_engine.c         mmap loader, ternary inference, axiom gate
├── src/main.c                 CLI
├── src/hydra_neon.h           ARM NEON kernel (actively integrated; scalar fallback on x86)
├── tools/make_model.py        Starter-model generator (format reference)
├── tools/gguf_reader.py       GGUF header/tensor reader (Python stdlib only)
├── tools/gguf_inspect.py      Read-only GGUF report (no numpy, no torch)
├── tools/gguf_to_hydra.py     GGUF → .hydra converter, absmean ternarisation
├── tools/gguf_test.py         Converter gate: synthetic GGUF in, real C engine out
├── tests/test_engine.c        71 unit tests (x86) / 77 on ARM (incl. NEON-vs-scalar, OOB + overflow PoC regressions)
├── android/                   NDK/JNI app wrapping the same C source
├── server.js                  UI server (Node, 0 runtime dependencies; ESLint is dev-only)
├── public/                    Hydra-Stone Console (HTML/CSS/JS)
├── docs/ARCHITECTURE.md       Architecture & math
├── docs/FORMAT.md             .hydra binary format specification
├── docs/GGUF-IMPORT.md        GGUF import, absmean ternarisation, limits
├── docs/ANDROID_SKILL.md      Android NDK/JNI integration guide (from experience)
├── .github/workflows/ci.yml       build, tests, ARM/NEON parity
├── .github/workflows/lint.yml     linters, sanitizers, CodeQL, Semgrep, Android Lint
├── .github/workflows/release.yml tag → installable APK + CLI binaries
├── errors.md                  Every bug: symptom / cause / fix / prevention
├── rules.md                   Standing working rules
├── session.md                 Live session plan and history
└── LICENSE                    MIT
```

## The Math in 60 Seconds

**Quantization (absmean):**
```
γ = 1 / mean(|W|)          W̃ = clip(round(γ · W), -1, +1)
```

**Forward step without multiplication:**
```
acc_i += (+1) · x_j   for every w_ij = +1
acc_i += (−1) · x_j   for every w_ij = −1
```

**On-disk footprint per parameter (v1): 4 bits** — two ternary weights per byte, upper 4 bits reserved. Measured against a naive FP16 checkpoint:

| Parameters | FP16 | INT4 (4 bits) | Hydra v1 (4 bits) | Hydra v2 (2 bits, planned) |
|---|---|---|---|---|
| 0.5 B | 1000 MiB | 250 MiB | 250 MiB | 125 MiB |
| 1.0 B | 2000 MiB | 500 MiB | 500 MiB | 250 MiB |
| 1.5 B | 3000 MiB (OOM) | 750 MiB | 750 MiB | 375 MiB |

The honest framing: **v1's win over FP16 is compression, not density parity.** It matches INT4 on disk and beats it in *compute* (additions only, no dequantization, no FP unit). The 2× edge over INT4 arrives with 4-weights-per-byte packing in v2 — see `docs/FORMAT.md`.

## Testing Strategy

The test suite goes beyond smoke checks — every test can fail:

- **Hand-computed decoder cases** — known weight bytes with hand-derived expected tokens (e.g. `w=0x01, token=5 → 11`); any packing bit-order or token-derivation error fails immediately.
- **NEON vs. scalar, same build** — on ARM, a scalar reference implementation of `step()` runs alongside the SIMD kernel over the same model; token sequences **and** state vectors must be bit-identical. This is what caught the NEON sign inversion described below.
- **Seeded roundtrip & variance** — 5 deterministic LCG seeds generate random ternary weight patterns; per seed, all 32 outputs must stay within vocab and a second run with a *fresh engine instance* must reproduce the bit-identical sequence. Sequences must differ across seeds (detects a no-op engine).
- **Security regressions** — crafted headers (bad magic, out-of-bounds weights, `layers × dim > weights_len`, `weights_offset` pointing into the header, `layers = 16 909 321` signed-overflow PoC) must be rejected at load. Every one of these was a real, reproduced bug first.
- **Axiom edge cases** — `NaN`, `±Inf` and negative `humanity_factor` must all block (`NaN ≤ 0` is false in IEEE-754, so the finiteness check is load-bearing).
- **Cross-platform determinism** — verified empirically: host x86-64 (scalar), ARM64 under NEON, and the Android emulator all produce the identical token sequence for the same model.

## Running the Web Console

The server itself has **zero runtime npm dependencies**. ESLint is a dev-only dependency used by the lint gate.

```bash
npm ci        # only needed to run the linter
npm run lint
make ui       # builds hydra-run + the starter model and serves on :8787
```

## Roadmap

- [ ] Full transformer forward pass (RMSNorm, RoPE, SwiGLU) on top of the ternary core
- [ ] Min-P sampling & repetition penalty
- [ ] safetensors import with automatic absmean ternarization (GGUF is done — see [`docs/GGUF-IMPORT.md`](docs/GGUF-IMPORT.md))
- [ ] AVX2/AVX-512 LUT kernel for x86 (current x86 path: purely scalar)
- [ ] 4-weights-per-byte packing (true 2 bits/weight) and an aggregated `A[i]/B[i]` model format — see the redundancy analysis in `docs/ARCHITECTURE.md`
- [ ] Streaming ring-buffer KV with attention sinks

Contributions welcome — see `docs/FORMAT.md` for the binary spec and dive in.

## License

MIT — see [LICENSE](LICENSE).
