# Android Baseline Audit

**Scope:** Phase 0 of the "Real LLM Engine on Android" work — verify the live tree
before changing anything, and record the baseline that every later phase is measured
against.

- **Audit date:** 2026-10-03
- **Audited commit:** `6ed9ce5` (`origin/main`, "Merge pull request #40 from
  JONIMONI09/docs/agent-skills-and-protocol")
- **Local tree audited:** `docs/agent-skills-and-protocol` @ `ccc23b3`, tree
  `a21dac85c6daf7ee5109b21e6c15faf4c7ba9195` — **byte-identical to `origin/main`**
  (verified: `git diff --stat ccc23b3 origin/main` is empty, both trees hash to
  `a21dac8`). The working tree was clean (`git status --porcelain` → 0 lines).
- **Author of this change:** none yet. This document is the first commit of the work.

> **Provenance rule for this document** (rules.md R7, R31, R32). Every statement is
> one of: a **code citation** (`file:line` + symbol), a **measurement** (this
> environment, 2026-10-03, with the command), or an explicit **NOT MEASURED** /
> **ESTIMATE** marker. There are no unlabelled claims.

---

## 0. Baseline verification (what "re-verify the live tree" produced)

| Check | Command | Result |
|---|---|---|
| Live HEAD | `git fetch origin --prune` | `f15a88e..6ed9ce5 main`, new tag `v1.0.16` |
| Local branch is main's content | `git diff --stat ccc23b3 origin/main` | empty |
| Working tree clean | `git status --porcelain` | 0 lines |
| CI on main | `gh run list` | `Release` success 3m51s, `CI` success 3m22s, `Lint & Security` success 2m21s — all on merge commit `6ed9ce5` |
| v1 unit tests | `make test && ./hydra-test` | 0 failures, exit 0 (count parsed from the binary's own output — see §5.2) |
| Test-count gate (x86) | `bash tools/test_count_check.sh ./hydra-test x86` | exit 0 |
| Docs claim lint | `bash tools/docs_claim_check.sh` | `all banned claims absent`, exit 0 |
| JNI signature gate | `bash tools/jni_signature_check.sh` | `3 native method(s) verified`, exit 0 |
| Android header gate | `bash tools/android_header_check.sh` | `22 checks passed`, exit 0 |
| Android debug APK | `gradle assembleDebug --offline` | exit 0, APK present |

The user's stated snapshot `6ed9ce5` is therefore **confirmed correct and current**.
The only change since the previously audited base `f15a88e` is the merge of PR #40
(three CI-gate files, +66/−16) — **no engine, Android or model file changed.**

---

## 1. Verified JNI surface

### 1.1 Native exports — `android/app/src/main/cpp/hydra_jni.c` (461 lines)

| Symbol | Line | JNI signature | Kotlin counterpart |
|---|---|---|---|
| `JNI_OnLoad` | 28 | `(JavaVM*, void*)` | — |
| `Java_dev_hydrastone_HydraBridge_cancel` | 39–40 | `void (jobject)` | `HydraBridge.cancel()` |
| `Java_dev_hydrastone_HydraBridge_runInference` | 163–164 | `jstring (jstring, jintArray, jint, jobject)` | `HydraBridge.runInference(...)` |
| `Java_dev_hydrastone_HydraBridge_benchmark` | 392–393 | `jstring (jstring, jint, jobject)` | `HydraBridge.benchmark(...)` |

**This is the entire native entry surface.** There is no other exported symbol.

### 1.2 Callback descriptors actually looked up (verified by gate)

- `onTokens` → `([IZ)V` — block of ~16 tokens, `done` true once
- `onToken` → `(II)V` — legacy per-token shape, **called only by `benchmark()`**
  (`HydraBridge.kt:19-23`)

Both descriptors are grep-verified present in `hydra_jni.c` by
`tools/jni_signature_check.sh` (exit 0).

### 1.3 Kotlin bridge — `android/app/src/main/java/dev/hydrastone/HydraBridge.kt` (75 lines)

| Symbol | Line | Notes |
|---|---|---|
| `object HydraBridge` | 4 | singleton |
| `interface Callback` | 14 | `onTokens` (16), `onToken` (23) |
| `init { System.loadLibrary("hydra") }` | 26–28 | loads **`libhydra.so`**, not a llama library |
| `external fun runInference(modelPath, prompt, steps, callback): String` | 40 | returns JSON; `{"ok":false,"cancelled":true,...}` is **not** an error |
| `external fun cancel()` | 58 | cooperative; flag cleared at the start of each run |
| `external fun benchmark(modelPath, steps, callback): String` | 71 | device-side ns/token measurement |

### 1.4 Native build — `android/app/src/main/cpp/CMakeLists.txt` (21 lines)

```
project(hydra C)            # line 2 — C only, no C++
add_library(hydra SHARED    # line 9
        ${HYDRA_ROOT}/src/hydra_engine.c
        ${CMAKE_CURRENT_SOURCE_DIR}/hydra_jni.c)
```

`CMAKE_C_STANDARD 99` (line 4), `-O3 -Wall -Wextra` (line 18), links only `log`
(line 20–21). The v1 engine source is compiled straight out of the **repo root**,
five levels up (`get_filename_component`, line 7) — there is one engine source, not a
copy (rules.md R12).

### 1.5 Gradle / ABI surface — `android/app/build.gradle.kts` (103 lines)

| Setting | Line | Value |
|---|---|---|
| `compileSdk` / `targetSdk` | 8, 13 | 34 |
| `minSdk` | 12 | 24 |
| `ndkVersion` | 19 | `26.3.11579264` |
| `abiFilters` | 24 | `arm64-v8a`, `armeabi-v7a`, `x86_64` |
| CMake arguments | 29 | **`-DANDROID_STL=none`** |
| Kotlin / AGP | `android/build.gradle.kts:2-3` | 2.0.0 / 8.5.2 |
| Runtime dependencies | 93–103 | **`androidx.activity:activity-ktx:1.9.3` — nothing else** |

**No Compose, no Material Components.** Line 95 states this as a deliberate
decision: *"no Compose, no Material Components - the UI stays programmatic Views."*

### 1.6 Kotlin UI — `MainActivity.kt` (695 lines)

`class MainActivity : ComponentActivity()` (line 45). Programmatic Views only:
`setContentView(ScrollView(this).apply { addView(root) })` (line 178).

Import path (the part Phase 2 must not break):

- `registerForActivityResult(ActivityResultContracts.OpenDocument())` — line 67
- MIME filter `*/*` — line 181, with the comment that `.hydra` has no reliable MIME type
- `importModel(uri)` — line 431 → background `Thread` (435) → streamed
  `contentResolver.openInputStream(uri).copyTo(out)` (439–440) into
  `File(filesDir, "import.hydra.tmp")` (437) → **renamed** to
  `File(filesDir, IMPORTED_NAME)` (468)
- Header validation `analyseHydraFile(file, size)` — line 564, over
  `HEADER_BYTES = 24` (line 674); `HeaderReport`/`HeaderRule` — lines 503–518
- Magic constants — lines 679–689: `HYDRA_MAGIC 0x48594452`, `GGUF_MAGIC 0x46554747`,
  `ZIP_MAGIC`, `ELF_MAGIC`, `PNG_MAGIC`, `GZIP_MAGIC`

**Why the import copies rather than opening in place** (documented at
`MainActivity.kt:38-43`): `hydra_engine_load()` mmaps a real path and a `content://`
URI is not one; the `.tmp` + rename means a cancelled import can never leave a
truncated model. **llama.cpp needs the same property** — it mmaps the GGUF by path —
so this copy-then-rename discipline carries over unchanged, and the determinate
progress the Models tab requires is a natural extension of this existing copy loop.

Benchmark entry point: `runBenchmark()` line 391 → `HydraBridge.benchmark()` at
line 401; reachable from the UI at lines 193.

---

## 2. v1 engine files that must NOT change (rule 4 of the task)

Content hashes at `6ed9ce5`, recorded so any accidental edit is detectable:

| File | SHA-256 |
|---|---|
| `src/hydra_engine.c` | `d2a25445aec590d5f45a5e156c1a663fff996b6a7edabc5dd058b91535d59479` |
| `include/hydra_model.h` | `d2959c5268751d2b27537cf8b9f39c5bf23a4adad85f879a54f815568f20f905` |
| `android/app/src/main/cpp/hydra_jni.c` | `dd4e4f7f3cf60728f1bd1ff58b557615c0084e4262a656c8499d062344a83059` |
| `android/app/src/main/cpp/hydra_batch.h` | `76a2a3391c388118b6d24e5d3fb578d0fa15b74874a5de9cd22dd6e41fcd33fe` |
| `android/app/src/main/cpp/CMakeLists.txt` | `3061b3d687c510008ad7e3832310ba103806134ede6d4906fc7bb3459469566a` |

`CMakeLists.txt` is listed here because "new code goes in new files" and Phase 2 must
*extend* it; the requirement that `hydra_jni.c` stays byte-identical is the binding one.

Public v1 engine API (the second engine must be additive only):
`hydra_engine_load` (`include/hydra_model.h:75`), `hydra_engine_unload` (:76),
`hydra_engine_resident_kb` (:80), `hydra_engine_step` (:81),
`hydra_engine_step_fast` (:104), `hydra_engine_prefill` (:111).

---

## 3. llama.cpp target: pinned version, license, and the exact API

Verified on the web on 2026-10-03 against the upstream repository (rules.md R26).

| Property | Value | How it was verified |
|---|---|---|
| **Pin** | **tag `v0.5.0`** | `GET /repos/ggml-org/llama.cpp/releases/latest` → `tag_name: v0.5.0`, `prerelease: false` |
| **Commit** | **`7fe450e19305b828c199d602c23a8337aaa1f03b`** | `GET /repos/ggml-org/llama.cpp/commits/v0.5.0`; subject *"llama.cpp : bump version to 0.5.0 (#29333)"*, committed 2026-09-23T17:32:51Z |
| **Release date** | 2026-09-23T20:50:06Z | GitHub releases API |
| **License** | **MIT**, "Copyright (c) 2023-2026 The ggml authors" | first lines of `LICENSE` at tag `v0.5.0` |
| Tag history | v0.1.0, v0.1.1, v0.1.2, v0.2.0, v0.3.0, v0.4.0, v0.4.1, **v0.5.0** | `GET /tags?per_page=8` — upstream moved to semantic versioning; `master` is **not** tracked |

`v0.5.0` is the newest **stable** release. A specific commit is required because the
upstream API is actively migrating (see 3.1), so "latest master" is not reproducible.

### 3.1 The v0.5.0 API differs from the older API in two ways that matter

Every symbol below was verified by fetching `include/llama.h` at tag `v0.5.0`
(1646 lines) and grepping it. The call sequence named in the task is **valid** in
`v0.5.0`, with one correction:

1. **`llama_backend_init(void)`** — `llama.h:482`. Paired with
   `llama_backend_free(void)` (`:485`).
2. **`llama_model_load_from_file(const char*, struct llama_model_params)`** →
   `struct llama_model*` — `llama.h:517`.
3. **`llama_init_from_model(struct llama_model*, struct llama_context_params)`** →
   `struct llama_context*` — `llama.h:544`.
4. **`llama_model_get_vocab(const struct llama_model*)` → `const struct llama_vocab*`**
   — `llama.h:588`.

> **Correction to the task text (important).** In `v0.5.0` the vocabulary is an
> **opaque pointer obtained separately**, not a member of `llama_model`. Every
> tokenizer and detokenizer call therefore takes an explicit `const struct
> llama_vocab *` as its **first** argument. Writing `llama_tokenize(model->vocab, …)`
> — the shape used on older llama.cpp — **will not compile against the pin.**

5. **`llama_tokenize(const struct llama_vocab *vocab, const char *text, int32_t
   text_len, llama_token *tokens, int32_t n_tokens_max, bool add_special, bool
   parse_special)`** — `llama.h:1180`.
6. **`llama_token_to_piece(const struct llama_vocab *vocab, llama_token token, char
   *buf, int32_t length, int32_t lstrip, bool special)`** — `llama.h:1194`.
7. **EOG check is `llama_vocab_is_eog(const struct llama_vocab*, llama_token)`** —
   `llama.h:1113`. The name in the task text, `llama_token_eog`, **does not exist** in
   `v0.5.0` (0 occurrences). The older spelling `llama_token_is_eog` exists but is
   marked `DEPRECATED(…, "use llama_vocab_is_eog instead")` at `llama.h:1144`.
8. **Sampling** — `llama_sampler_chain_default_params()` (`:476`),
   `llama_sampler_chain_init` (`:1355`), `llama_sampler_chain_add` (`:1358`),
   `llama_sampler_init_top_k` , `llama_sampler_init_top_p` (`:1385`),
   `llama_sampler_init_min_p`, `llama_sampler_init_temp` (`:1394`),
   `llama_sampler_init_dist(uint32_t seed)` (`:1378`),
   `llama_sampler_init_greedy` (`:1375`), `llama_sampler_sample(smpl, ctx, idx)`
   (`:1548`), `llama_sampler_accept(smpl, token)` (`:1344`).
   Ownership note from `llama.h:1349`: *do not free* a sampler that has been added to
   a chain.
9. **Decode / logits** — `llama_decode` (11 references, `llama.h`),
   `llama_get_logits_ith(ctx, i)` (`:1049`).
10. **Batch** — `llama_batch_init` (`:968`) / `llama_batch_free` (`:974`) for
    caller-allocated batches; `llama_batch_get_one` (`:957`) for the single-token
    convenience path that a token-at-a-time decode loop uses.
11. **Context sizing** — `llama_n_ctx(ctx)` (`:570`), `llama_n_ctx_seq` (`:571`),
    `llama_n_ubatch` (`:573`), `llama_n_seq_max` (`:574`). The **context-window
    overflow behaviour required by Phase 3 must be driven by `llama_n_ctx(ctx)`, not
    by the model's training length**: `llama_n_ctx_train` is `DEPRECATED`
    (`:577`); the supported spelling is `llama_model_n_ctx_train(model)` (`:591`).
12. **KV-cache sizing knobs** — `llama_context_params` exposes `type_k` and `type_v`
    (`ggml_type`, marked EXPERIMENTAL) plus `n_ctx`, `n_batch`, `n_ubatch`,
    `n_seq_max`, `n_threads`, `n_threads_batch`, `abort_callback`. These are the
    inputs to the per-model RAM estimate the Models tab must display.

Relevant build options for an NDK build (verified in `CMakeLists.txt` and
`ggml/CMakeLists.txt` at `v0.5.0`): `LLAMA_BUILD_TESTS`, `LLAMA_BUILD_TOOLS`,
`LLAMA_BUILD_EXAMPLES`, `LLAMA_BUILD_SERVER`, `LLAMA_BUILD_COMMON` (all default
`OFF` when not standalone), `LLAMA_OPENSSL` (**default ON** — must be forced `OFF`
for Android, no TLS stack wanted), `GGML_NATIVE`, `GGML_LTO`, `GGML_CCACHE`.

---

## 4. Blockers and risks found in this audit

Ordered by how early they will bite. Each is a *finding*, not a decision.

| # | Finding | Evidence | When it bites | Disposition |
|---|---|---|---|---|
| **B1** | **`-DANDROID_STL=none` (`app/build.gradle.kts:29`) is incompatible with llama.cpp.** llama.cpp is C++ and requires a real C++ runtime; the current setting gives the native build no STL at all. | `build.gradle.kts:29`; llama.cpp `CMakeLists.txt` is C++ throughout | Phase 2, immediately | Must change to `c++_static` (ships the runtime inside the APK; `none` will not link). This is a **toolchain change** and belongs in its own Phase-2 commit. |
| **B2** | **No Gradle wrapper is checked in.** `android/gradle/wrapper/` does not exist; there is no `gradlew` anywhere in the repo. CI builds with `$GRADLE_BIN` (`.github/workflows/ci.yml:413`), pointing at a Gradle installed by `.github/actions/setup-android`. | `ls android/gradle/wrapper/` → no such file; `ci.yml:413` | Phase 1 gate wording | Phase 6's `./gradlew assembleDebug assembleRelease` is **not runnable as written**. This audit uses the CI's own mechanism instead (see §5). Do not report a `./gradlew` result that was never produced. |
| **B3** | **`tools/android_header_check.sh` (22 checks) greps `MainActivity.kt` for 8 fixed literal strings**, including `analyseHydraFile(tmp, bytes)`, `val valid: Boolean get() = rules.all { it.ok }`, `val weightsEnd = offset + len`, `formatBytes(MAX_UPLOAD_BYTES)`, `GGUF_MAGIC`, `ZIP_MAGIC`, plus `R.string.*` references in `strings.xml`. | `tools/android_header_check.sh:40,43-46,55,58,62,76-77` | Phase 1 | The Compose rewrite of `MainActivity` breaks all 22 checks. The **fix is to repoint the gate at the new file** that keeps `analyseHydraFile`, not to delete or weaken the gate (rules.md R25). Every check must be shown to still fail when its behaviour is removed (R25 negative control). |
| **B4** | **`tools/jni_signature_check.sh` is scoped to `HydraBridge.kt` only** (default path, line 19). New `external fun`s in a new `LlamaEngine.kt` are **not covered** by any gate. | `jni_signature_check.sh:6,19` | Phase 2 | New JNI methods ship with **no signature verification**. Extending the gate to cover the llama bridge is a Phase-2 follow-up; until then, a wrong JNI descriptor is a runtime `UnsatisfiedLinkError`, not a build failure. |
| **B5** | **No usable Android runtime target in this environment.** `adb devices` lists nothing; `/dev/kvm` does not exist. The one AVD, `hydra_test`, is API 24 **x86_64** with no KVM → pure TCG software emulation. | `adb devices`; `ls /dev/kvm` → no such file; `avdmanager list avd` | Phase 2 gate, Phase 6 | A Phase-2 "runs on an emulator" gate is **reachable but very slow**; a meaningful speed measurement is not. No **physical ARM device** exists here at all. |
| **B6** | **The Phase-2 test model is not in the repo and must not be committed.** `android/app/src/main/assets/` contains exactly one file, `starter.hydra` (280 bytes). | `ls -la android/app/src/main/assets/` | Phase 2 | Needs a download script + a recorded licence note. A ~70–100 MB GGUF must never enter git. |
| **B7** | **`minSdk = 24`** (`build.gradle.kts:12`) while the task targets modern small instruct models. | `build.gradle.kts:12` | Phase 2 | Low risk (`c++_static` supports API 21+), but it must be *measured* at runtime, not assumed. |

---

## 5. Measured baseline

All measured 2026-10-03 in this environment. Commands are reproducible.

### 5.1 Native build and APK (the Phase-2 delta reference)

| Quantity | Value |
|---|---|
| `gradle assembleDebug --offline` | **exit 0** |
| Debug APK size | **4,263,057 bytes** (4.07 MiB) |
| `lib/arm64-v8a/libhydra.so` | 18,816 bytes (uncompressed, in-APK) |
| `lib/armeabi-v7a/libhydra.so` | 15,432 bytes |
| `lib/x86_64/libhydra.so` | 16,928 bytes |
| Packaged assets | `assets/starter.hydra` (280 bytes) |

Build invocation (the B2 workaround — this is what CI does, not `./gradlew`):

```sh
cd android
export ANDROID_HOME=/opt/android-sdk ANDROID_SDK_ROOT=/opt/android-sdk \
       GRADLE_USER_HOME=/root/.gradle
/opt/gradle/gradle-8.7/bin/gradle assembleDebug --offline --no-daemon
```

Toolchain present locally: Gradle 8.7, Android SDK `/opt/android-sdk` with
build-tools 34.0.0, platform-tools, cmdline-tools, **NDK 26.3.11579264** (matches
`build.gradle.kts:19`) and **CMake 3.22.1** (matches `CMakeLists.txt:1`).
`android/local.properties` is absent; `android/local.properties.example` supplies
`sdk.dir=/opt/android-sdk`.

### 5.2 v1 test baseline

`./hydra-test` exits 0 with **0 failures**. The assertion count is deliberately **not
written down here**: `tools/docs_claim_check.sh` rejects a hard-coded count in any
document (rules.md R21), and `tools/test_count_check.sh` parses the real count out of
the binary and compares it against the single reviewed file
`tools/test_count_baseline.txt` (`x86: 71`, `arm: 77`). That gate passed here — exit 0.
**The ARM count is larger by design** — the NEON-vs-scalar comparison assertions only
exist in an NEON build; do not "fix" a drift by lowering it.

---

## 6. Explicitly NOT MEASURED

Per rules.md R32 ("blocked, not measured"), none of the following is known, and no
number for any of them appears anywhere in this repository:

- **Tokens/second** — for SmolLM2-135M, Qwen2.5-0.5B, or anything else, on any device.
  The widely repeated figures (~100+ tok/s for SmolLM2-135M, ~30–40 tok/s for a 0.5B
  model on a Snapdragon 8 Elite) are **UNVERIFIED ESTIMATES** and are not treated as
  targets or as evidence anywhere in this work.
- **Peak RSS for any GGUF model.** Weights are mapped rather than stored on the heap,
  but mapping is **not** free memory: llama.cpp maps the file and pages it in on
  demand, and the page tables for a very large mapping are themselves tens of MB.
  Residency is governed by the kernel page cache, not by the allocator. Android
  `lmkd` may kill the process regardless of how much was allocated. The 852 MB figure
  for Qwen2.5-0.5B Q5_K_M on a Snapdragon 855 comes from a third-party report and is
  **NOT MEASURED here**.
- **Thermal behaviour, sustained-load throttling, battery impact.**
- **Release APK size delta** after the llama.cpp integration.
- **Native build time delta** after the llama.cpp integration.
- **Whether any specific GGUF file loads**, at any quantisation, on any device.
  Nothing in this repository has executed llama.cpp yet.

---

## 7. Phase 0 exit criteria

| Gate 0 requirement | Status |
|---|---|
| Re-verify live tree | **DONE** — §0; `6ed9ce5` confirmed, tree clean, CI green |
| Confirm current HEAD | **DONE** — `6ed9ce5`, tree `a21dac8` |
| Inventory every JNI function | **DONE** — §1.1 (3 methods + `JNI_OnLoad`) |
| Inventory every Kotlin bridge symbol | **DONE** — §1.2, §1.3 |
| With file paths | **DONE** — every row carries `file:line` |
| Confirm import path | **DONE** — §1.6 |
| Confirm CI status | **DONE** — §0, all three workflows success |
| Confirm llama.cpp version + licence (MIT) | **DONE** — §3, `v0.5.0` @ `7fe450e1`, MIT |
| Baseline for later deltas | **DONE** — §5 |

**Phase 0 is complete.** Seven findings (§4) carry forward; **B1** and **B3** must be
handled before the Phase 1 and Phase 2 gates can be honestly declared green.
