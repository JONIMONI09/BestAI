# Hydra-Stone

**Ternary inference engine with no heap allocation for weights — drop in a `.hydra` model and go.**

```bash
./hydra-run my_model.hydra 123
```

A whole token sequence can be fed in as a prompt — every token before the last one is run through the engine as context, the last one is the seed for the generated continuation:

```bash
./hydra-run my_model.hydra 0 16 --prompt 7,9,11 --json
```

Hydra-Stone is a C99 engine that maps a `.hydra` file of ternary weights ({-1, 0, +1}) read-only via `mmap` and **aggregates them once at load time**. The inference step then consists purely of integer additions.

**Memory claim, stated honestly:** the engine performs **no heap allocation for weights** — `src/hydra_engine.c` contains no `malloc`/`calloc`/`realloc` at all, only `mmap`/`madvise`. What it does use is a **fixed working set that does not grow with model size**:

| Buffer | Type | Size | Lives in |
|---|---|---|---|
| recurrent state | `int8_t[64]` | 64 B | `HydraEngine.state_vector` |
| aggregate A | `int32_t[64]` | 256 B | `HydraEngine.agg_a` |
| aggregate B | `int32_t[64]` | 256 B | `HydraEngine.agg_b` |
| per-step accumulator | `int64_t[64]` | 512 B | stack in `hydra_engine_step()` |
| **total while stepping** | | **1088 B ≈ 1.06 KiB** | |
| load-time scratch (released) | `int64_t[64]` × 2 | 1024 B | stack in `hydra_build_aggregation()` |

`sizeof(HydraEngine)` is 656 B in total — the three arrays above plus the file descriptor, the mapping pointer, the 24-byte header and the load-measurement fields. Nothing in that list grows with `layers` or with the file size. Full derivation and the guarantees/limits are in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) § *Memory Model*.

The weight pages are **not** counted there, because they are not the engine's memory: they live in the **kernel page cache**, appear in `dumpsys meminfo` as *Private Clean*, and are reclaimed by the OS under memory pressure. The engine therefore cannot and does not keep the model's resident set size at zero. Build with `-DHYDRA_DROP_CACHE` to explicitly hand touched pages back after every step.

---

## Why It's Fast

| Approach | Multiply-adds/Token | RAM Growth | Typical Bottleneck |
|---|---|---|---|
| FP32 Transformer | O(N² · d) | KV cache grows unbounded | HBM bandwidth |
| INT8 quantization | O(N² · d) | KV cache grows unbounded | Dequantization overhead |
| **Hydra-Stone (ternary + mmap)** | **`2·d`, independent of depth** | **fixed-size engine buffers (~1.06 KiB), independent of model size; mapped weight pages are page-cache-backed and count toward physical memory** | disk/cache bandwidth only |

Three levers:

1. **Ternary weights** — in the inner loop the weight is one of `{-1, 0, +1}`, so no FP multiply is needed: `y = Σ_{w=+1} x − Σ_{w=−1} x`. Layer aggregation then collapses this to exactly `2·d` integer multiply-adds per token (128 at `d = 64`) — **constant in the number of layers**, which is the claim that matters. Saying "0 multiplications" would be false: the aggregated form does multiply, just not by depth.
2. **mmap paging** — the kernel faults in only the 4-KiB pages the compute cursor actually touches; clean pages are reclaimable under memory pressure (no swap traffic).
3. **Fixed-size recurrent state** — instead of a growing KV cache, a fixed `int8_t[64]` state vector plus the two `int32_t[64]` load-time aggregates and an `int64_t[64]` per-step accumulator carry the whole working set (~1.06 KiB, see the memory claim above).

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

# 4. Unit tests (count is architecture-dependent - ARM runs the extra NEON-vs-scalar tests)
make test-run

# 5. Start the web console (desktop-optimized)
make ui    # → http://localhost:8787
```

## Build, Test, Deploy

Everything below runs from the repository root unless stated otherwise.

### Build

| Target | Command | Produces |
|---|---|---|
| C engine + CLI | `make` | `hydra-run` |
| Unit tests | `make test-run` | `hydra-test`, `jni-batch-test`, `large-model-test` |
| Web console | `make ui` | serves `http://localhost:8787` (also `node server.js`) |
| Android debug APK | `cd android && gradle assembleDebug` | `app/build/outputs/apk/debug/app-debug.apk` |
| Android release APK | `cd android && gradle assembleRelease` | `app/build/outputs/apk/release/app-release.apk` |
| Starter model | `python3 tools/make_model.py <out.hydra>` | a valid `.hydra` |

Android builds need JDK 17, the Android SDK, CMake and the NDK; set
`ANDROID_HOME` (see `android/README.md` for the exact versions).

### Test

```bash
make test-run                       # count is architecture-dependent; see tools/test_count_baseline.txt
python3 tools/gguf_test.py          # GGUF -> .hydra, checked against the real engine
node --test tools/server_test.js    # web console API and diagnostics
npm ci && npm run lint              # ESLint (the only dev dependency)
```

Static gates, all reproducible locally: `gcc -fanalyzer`, `clang-tidy`,
`cppcheck`, `flawfinder`, ASan + UBSan. The shell gates are
`tools/android_autostart_check.sh`, `tools/android_header_check.sh`,
`tools/android_crash_check.sh` and `tools/jni_signature_check.sh`.

**Cross-platform note:** the same 32-token sequence must come out on x86-64
(scalar), on ARM64 with the NEON kernel, and on the Android emulator. The
starter model is generated so that this is a real test and not a tautology —
see “Cross-platform determinism” below.

### Deploy

There are two deployment paths.

**The web console** is a single Node process with zero runtime dependencies:

```bash
node server.js                      # binds 127.0.0.1 only
HYDRA_ALLOW_REMOTE=1 node server.js # opt in to LAN access explicitly
```

**Releases** are published automatically by `.github/workflows/release.yml`
whenever `main` is pushed: it resolves the next version, creates the tag if
needed, and attaches the CLI binaries, the signed APK and `SHA256SUMS.txt`.
A merge to `main` therefore *does* publish a release — see the versioning rules
below. `workflow_dispatch` gives a build-only dry run when **tag** is left
empty and **publish** is unticked.

**APK signing.** Without repository secrets the release APK is signed with the
**debug key**: installable by sideloading, not Play-Store-ready. The build
never fabricates a release key. To sign for real, configure
`HYDRA_KEYSTORE_BASE64`, `HYDRA_KEYSTORE_PASSWORD`, `HYDRA_KEY_ALIAS` and
`HYDRA_KEY_PASSWORD`; the workflow decodes the keystore only when the value is
non-blank.

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

- ✅ **Zero-heap inference** — weights are never copied, only mapped; `src/hydra_engine.c` contains no allocator call in any path, and the engine's own working set is a fixed ~1.06 KiB
- ✅ **Ternary linear math** — ternary weights, no FP multiplication in the inner loop. *Density note:* the v1 format stores **2 ternary weights per byte — 2 bits per weight on disk** (`w1` in bits 0–1, `w2` in bits 2–3, upper 4 bits reserved). The planned v2 packs 4 weights per byte at the same 2 bits per weight, which **halves the file** by removing the wasted half of each byte — see `docs/FORMAT.md`.
- ✅ **GGUF import** — a `.gguf` is detected and ternarised into a real `.hydra` with absmean scaling, streaming so a multi-GB file does not have to fit in RAM. Decodes **F32, F16, BF16 and Q8_0**; every other GGUF block layout is refused by name. One import at a time — a concurrent one gets HTTP 429. Python stdlib only: no numpy, no torch. See [`docs/GGUF-IMPORT.md`](docs/GGUF-IMPORT.md).
- ✅ **Coexistence axiom** — `humanity ≤ 0 ∨ NaN ∨ ±∞ ⇒ Utility = −∞`, hard safety gate before any action
- ✅ **NEON SIMD kernel** — 16 parallel ternary accumulations, **actively integrated** into `hydra_engine_step()` on ARM (`__ARM_NEON`); bit-identical scalar fallback on x86
  - *Honesty note:* the NEON path is active on ARM builds only. It is **not** merely covered indirectly — `tests/test_engine.c` runs a scalar reference implementation next to the NEON kernel **in the same ARM build** and requires bit-identical token sequences *and* state vectors. The x86 path is purely scalar; AVX2 is on the roadmap.
- ✅ **Hardening** — header validation (little-endian decode, layer cap, offset checks), OOB protection, overflow-free accumulation; unit-test count is parsed from the binary and drift-gated in CI (`tools/test_count_check.sh`, baselines in `tools/test_count_baseline.txt`)
- ✅ **Web console** — desktop UI with live visualization (`make ui`)
- ✅ **Android** — NDK/JNI build of the same C source. Verified end to end on an API-24 x86_64 emulator with the **signed release APK**: the installed package is byte-identical to the built artifact, `lib/x86_64/libhydra.so` is genuinely mapped into the running process (ELF header `7f 45 4c 46`, `e_machine = EM_X86_64`), and the 32-token output is **bit-identical** to the host C engine. *Not yet verified on physical ARM hardware — no device was available.*
- ✅ **C99, zero dependencies** — runs on 32-bit ARMv7, x86-64, and everything in between

## Releases & the Android APK

Every `v*.*.*` tag produces a GitHub Release containing a **directly installable APK**:

```bash
# Install on a phone / emulator (Android 7.0 / API 24+)
adb install -r hydra-stone-<version>-android-arm64v8a-armeabiv7a-x86_64.apk
```

The APK bundles the same C engine (`libhydra.so` for arm64-v8a, armeabi-v7a and x86_64 — NEON active on ARM), the packed starter model (`assets/starter.hydra`, 280 bytes), and a launcher icon. The app **runs by itself** on launch — it copies the starter model into private storage and starts an inference run; untick *Run automatically on start* to skip that. Tapping **Run inference** executes the engine again and streams the tokens live.

**Importing your own model.** *Import model…* opens the system file picker (Storage Access Framework, `*/*` because `.hydra` has no reliable MIME mapping). The chosen file is streamed into the app's private storage, validated against the same header rules the C loader enforces, and only then renamed into place and used — the starter model stays as the fallback when nothing valid has been imported. Tokens are delivered from the JNI layer in blocks of 16 instead of one call per token. *Measured caveat:* on a software-emulated (TCG) emulator the in-app benchmark reported `batching_saves_per_token` as **negative** — under emulation the engine work dominates and the JNI transition is proportionally cheap. The batching policy itself is verified deterministically by `tests/test_jni_batch.c`; whether batching is a net win is a device question, not yet settled on real hardware.

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

**Every push builds the APK.** A dedicated `android-apk` job in `ci.yml` runs on every push and pull request: it builds `assembleDebug` and `assembleRelease`, verifies the signature with `apksigner`, checks that `libhydra.so` is packaged for arm64-v8a, armeabi-v7a and x86_64 together with the model asset, and uploads the APK as a workflow artifact. A release is published for a `v*.*.*` tag **and** automatically on every push to `main` (see below) — but a broken app build can therefore never reach release day unnoticed. The JNI bridge is checked separately by `jni-signatures`: `javac -h` derives the JNI header from the Kotlin declarations and `tools/jni_signature_check.sh` proves every native method exists in the C bridge with a matching parameter list.

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
| **GGUF import** | **Import .gguf** converts a llama.cpp/Ollama checkpoint into `.hydra` (`POST /api/models/import?name=…`) with absmean scaling, and returns a conversion report: which source tensor fed each plane, its shape, rows read vs. rows sampled, the **absmean scale `gamma`** (the reciprocal of the mean absolute weight), how many weights ternarised to zero, the resulting density, the rescale factor, the resulting peak and the vocabulary size — plus the reason if it refused. The converted model appears in the chat model selector marked as converted, and the source vocabulary is carried over so chat words map to real token ids. |
| **Diagnostics** | `GET /api/models/inspect?path=models/starter.hydra` returns the header values and one entry per rule (`id`, `ok`, `message`) plus a `violations` list. It is the same analysis the upload route uses, so both quote the same reason. Known formats are **named**: a GGUF answers *"this is a GGUF model (llama.cpp / Ollama) — the engine reads .hydra files"* instead of a bare `wrong magic`. The report also states the ceiling of the current format (`limits.maxWeightsBytesForShape`), because "valid" and "can be 20 GB" are two different questions. The console shows the violated rules under any model marked *invalid header*. |
| **Engine output** | Token chart with metrics; the state vector and the raw log are collapsed by default. |
| **Training** | Paste a corpus (`3 3 3 3 -> 7 7 7 7`, or `1 2 3` for next-token training), pick vocab/dim/layers/epochs and train. Training writes a real `.hydra` file and is **verified against the compiled engine** before it is reported as successful; accuracy before/after is measured, never estimated. |
| **Settings** | Start token, steps, coexistence-axiom factor, per-model vocabulary editor. The drawer traps focus, closes on Escape and returns focus to its toggle. |

Only one inference runs at a time: **Send** and **Train** are disabled while a request is in flight and a **Cancel** button aborts the request. Cancel is real, not cosmetic — the server kills the engine process it started (`SIGTERM`, then `SIGKILL` after a grace period) and answers **409 `cancelled`**. Because each request owns its own child process, killing it cannot disturb another request. A client that simply disconnects is treated the same way.

Everything shown comes from the engine. There is no simulated output anywhere in the console.

**How the trainer works.** The engine's update is `acc[i] = Σ (w1·token + w2·state[i])` with the next token decoded from `acc[0]`. Only column 0 influences the decoder, so the trainer searches ternary moves there (`0/±1`), accepts a move only when replaying the **complete** sample afterwards yields more correct steps, and returns the best snapshot over all epochs. Accuracy therefore never drops below the seed weights. The engine has a small capacity by design — the console reports the real number instead of hiding it.

## Performance

**Measurement provenance for every number below (rule R32).** These were taken
on a **shared x86-64 Linux container**, Intel Xeon @ 2.60 GHz, 2 vCPU, 3.9 GB
RAM — *not* on a phone or tablet, and not on the target Snapdragon 8 Elite. No
Android device measurement exists for this project; where one is needed the
text says "not measured". Re-run on your own hardware before quoting them.

The inference step is now two multiply-adds per dimension, not two per
(layer x dimension):

```
python3 tools/make_model.py models/starter.hydra
./hydra-run models/starter.hydra 0 --bench 50000
{"mode":"bench","engine":"cpu","model":"models/starter.hydra","dim":64,"layers":4,
 "vocab":512,"steps":50000,"warmup_layers":4,"ns_per_token_full":137.03,
 "ns_per_token_fast":16.49,"speedup":8.311,"tokens_equal":true,"load_ms":0.015,
 "weights_bytes":256,"rss_before_kb":948,"rss_after_kb":948,"rss_delta_kb":0,
 "note":"JNI callback costs are NOT included here; measure them with HydraBridge.benchmark() on the device"}
```

`load_ms` and `rss_delta_kb` are measured, not asserted: the loader reports how long the map took and how much resident memory the run added. **The `rss_delta_kb: 0` above holds for the 256-byte starter model shown**, whose entire weight region fits inside the existing mapping — it is not a general statement about larger models, whose weight pages are real physical memory until the kernel reclaims them.

The `ns_per_token_*` figures are from one warm run on an idle host and move noticeably when the machine is busy (repeat runs on the same box gave 141.90 / 15.96 and 160.41 / 26.44). They are a sample, not a specification; take the median of a few runs. `load_ms` and `rss_delta_kb` are the stable results — sub-millisecond mapping and zero RSS growth regardless of timing noise.

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
| NEON | AArch64 cross-build under qemu + scalar/NEON parity check | platform-divergent SIMD |
| Portability | full build + engine tests on native x86-64 (`ubuntu-latest`) **and native ARM64** (`macos-latest`, an Apple Silicon runner) | a bug that only appears on 64-bit ARM |
| Release | signed release APK + x86-64/ARM64/macOS binaries, `apksigner` verification, `SHA256SUMS.txt` | shipping an unsigned or truncated artifact |
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
├── tests/test_engine.c        unit tests (NEON-vs-scalar, OOB + overflow PoC regressions); count is architecture-dependent and drift-gated
├── android/                   NDK/JNI app wrapping the same C source
├── server.js                  UI server (Node, 0 runtime dependencies; ESLint is dev-only)
├── public/                    Hydra-Stone Console (HTML/CSS/JS)
├── docs/ARCHITECTURE.md       Architecture & math
├── docs/FORMAT.md             .hydra binary format specification
├── docs/GGUF-IMPORT.md        GGUF import, absmean ternarisation, limits
├── docs/FORMAT-V2.md          .hydra2 sparse-MoE format — CANONICAL v2 spec, no loader yet
├── docs/HYDRA2-RESEARCH.md    research spec: bounded measured memory, load strategies, ternary math
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

**On-disk footprint per weight (v1): 2 bits** — two ternary weights per byte
(`w1` in bits 0–1, `w2` in bits 2–3), upper 4 bits reserved.

The table below is a **bits-per-parameter density comparison only**. It is
*not* a statement that Hydra-Stone can store models of that size:

| Parameters | FP16 | INT4 (4 bits) | v1 density (2 bits) | v2 packing (same 2 bits) |
|---|---|---|---|---|
| 0.5 B | 1000 MiB | 250 MiB | **125 MiB** | 62.5 MiB |
| 1.0 B | 2000 MiB | 500 MiB | **250 MiB** | 125 MiB |
| 1.5 B | 3000 MiB (OOM) | 750 MiB | **375 MiB** | 187.5 MiB |

**What v1 can actually hold:** `dim <= 64`, `layers <= 4096`, `vocab <= 1024`,
which caps the weight region at **256 KiB** (`64 x 4096` bytes). A parameter
count far beyond a few million is not expressible in v1 at all — that is a
format ceiling, not a memory problem (a 5 GiB sparse `.hydra` maps and
generates fine; `tests/test_large_model.c`). Lifting the ceiling is the job of
the v2 container, specified in `docs/FORMAT-V2.md`.

The honest framing: **v1 is INT2 on disk — it halves INT4, and beats it in
compute too** (additions only, no dequantization, no FP unit). Two honest
caveats. First, v1 wastes half of every byte: the reserved codes mean a byte
carries `log₂9 ≈ 3.17` bits of information, i.e. **1.58 bits per weight** — so
the real figure is *better* than the allocated 2. Second, v2's
4-weights-per-byte packing does **not** change the 2 bits/weight of weight data;
it only removes the wasted half of each byte, halving the file. See
`docs/FORMAT.md`.

## Testing Strategy

The test suite goes beyond smoke checks — every test can fail:

- **Hand-computed decoder cases** — known weight bytes with hand-derived expected tokens (e.g. `w=0x01, token=5 → 11`); any packing bit-order or token-derivation error fails immediately.
- **NEON vs. scalar, same build** — on ARM, a scalar reference implementation of `step()` runs alongside the SIMD kernel over the same model; token sequences **and** state vectors must be bit-identical. This is what caught the NEON sign inversion described below.
- **Seeded roundtrip & variance** — 5 deterministic LCG seeds generate random ternary weight patterns; per seed, all 32 outputs must stay within vocab and a second run with a *fresh engine instance* must reproduce the bit-identical sequence. Sequences must differ across seeds (detects a no-op engine).
- **Security regressions** — crafted headers (bad magic, out-of-bounds weights, `layers × dim > weights_len`, `weights_offset` pointing into the header, `layers = 16 909 321` signed-overflow PoC) must be rejected at load. Every one of these was a real, reproduced bug first.
- **Axiom edge cases** — `NaN`, `±Inf` and negative `humanity_factor` must all block (`NaN ≤ 0` is false in IEEE-754, so the finiteness check is load-bearing).
- **Cross-platform determinism** — verified empirically and pinned to a
  concrete sequence: host x86-64 (scalar), ARM64/NEON, and the Android emulator
  all emit `[471, 386, 385, 386, 385, 386, 385, 386]` for the starter model with
  `start_token=42` and 8 steps. CI enforces the x86-64/ARM64 half of this on every
  push (`neon-test` compares a scalar reference against the NEON kernel *inside
  the same ARM binary*); the Android half is covered by the emulator evidence
  recorded in `android/README.md`.
  *This is a real test, not a tautology:* the starter model is generated so that
  its column-0 sums are non-zero (`A[0] != 0` and `B[0] != 0`). An earlier
  version had `B[0] == 0`, which made it **state-blind** — it looped
  `471, 42, 471, 42` forever, and `--prompt` provably could not change the
  output. `tools/make_model.py` now guarantees and asserts the invariant.

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
- [ ] 4-weights-per-byte packing (same 2 bits/weight, but no wasted byte) — a v1.1 file-level optimisation, not the v2 container
- [ ] The `.hydra2` sparse-MoE container — the canonical v2 spec is [`docs/FORMAT-V2.md`](docs/FORMAT-V2.md); **no part of it is implemented**, and the aggregated `A[i]/B[i]` container sketched in `docs/ARCHITECTURE.md` is a superseded alternative, see its *Roadmap status* section
- [ ] Streaming ring-buffer KV with attention sinks

Contributions welcome — see `docs/FORMAT.md` for the binary spec and dive in.

## License

MIT — see [LICENSE](LICENSE).
