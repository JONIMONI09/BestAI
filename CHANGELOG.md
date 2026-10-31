# Changelog
All notable changes to this project, per change, are recorded here and in
`.agents/session.md` (.agents/rules.md R2). Newest entries first.

## 2026-10-31 (round 15) — large-model analysis (700B→8B), 2026 technique scan, README architecture diagram, Q4_0/Q4_1 GGUF import

Owner round: analyse how a 700B model reaches every device (or why not),
search the current (2026) techniques and integrate what fits, add the
final Mermaid architecture diagram to the main README (GitHub-rendered),
extend the test pipeline toward 8B-class models, docs.

1. **Q4_0 + Q4_1 GGUF import (the 8B-class quants).** `tools/gguf_reader.py`
   now decodes six layouts (F32/F16/BF16/Q8_0/Q4_0/Q4_1): the 4-bit pair
   follows ggml's `block_q4_0`/`block_q4_1` exactly (`w = d*(q-8)` /
   `w = d*(q-8)+m`, low-nibble-first packing) with C-speed decoders
   (strided gather + `bytes.translate` + `array` maps - no Python value
   loop). `gguf_minimal_llama.py` gains `--dtype q40/q41`. The v2
   converter therefore takes a real 8B Q4_0 GGUF (4.9 GB) without a
   `--convert-f16` re-export; the `.hydra2` output stays F32-resident
   (the container's own `Q4_0` type `0x0030` stays reserved - the
   4-bit-resident engine type is the next engine milestone, documented in
   `docs/LARGE-MODELS.md`). Gates: converter 35 -> **44 checks**
   (Q4_0/Q4_1 convert + run + deterministic, quantisation visibly changes
   the stream vs F32, embedded F32 bit-exact against an independent
   from-scratch block decode); `gguf_test.py` 101/0 (its two
   "undecodable" negative controls moved from Q4_0 to Q4_K - a control
   must never encode the behaviour under test).
2. **`docs/LARGE-MODELS.md` (new).** The honest 700B answer: 350 GB at
   4-bit, 185 GB at 2-bit - no device path exists, and "no loss" is the
   F32 reference, so the product answer is a precision ladder (Q8 server /
   Q4 phone / 2-bit small) + QAT for the Q4 gap; "no lag" is a bandwidth
   story (8B Q4 = 4-15 tok/s on 2026 flagships). Memory table
   8B→700B x F32/16/8/4/2-bit; 2026 technique scan with per-technique
   verdicts (MXFP4/NVFP4 landed in llama.cpp March-April 2026 - adopted at
   the converter boundary now, `T4_0` engine type next; KV INT8/FP8 =
   round-16 candidate; speculative decoding = best fit, v1 ternary as
   draft for the v2 target; prefix caching = cheap server win; NPU
   QNN/LiteRT-LM = the phone no-lag answer); the concrete 8B path with a
   4-step phone milestone list; the 120B-MoE showcase case (expert
   streaming is its access pattern); the hybrid 8B-local + big-server
   architecture; the Android section (nothing breaks this round; the app
   already runs 8B GGUFs via the bundled llama.cpp); sources.
3. **README:** new `## Architecture` section - one Mermaid flowchart of
   the whole system (sources -> converter -> .hydra2 -> MoE/v1 engines ->
   CLI/web/Android -> gate wall), rendered natively by GitHub; honest
   v1-vs-v2 paragraph; converter bullet now names the six decoded layouts
   and the 44-check gate.
4. **Docs sync:** `docs/GGUF-IMPORT.md` (reader table, Q4 layout
   descriptions, §9, §12, gate paragraph), `docs/FORMAT-V2.md` (decoder
   paragraph + import-path sentence; v2 container type table unchanged),
   `tools/gguf_to_hydra.py` refusal messages now name all six layouts.

Verified (local sandbox 2026-10-31: Linux e2b.local 6.1.158+ x86_64,
gcc 12.2.0, Python 3.11.2): hydra 234/0 (x86 baseline 234, count gate),
jni 128/0, large 7/0, hydra2 152/0, hydra-moe 180/0, gguf_test 101/0,
converter 44/0, text_prompt_check ok, server_test 47/0, workflow gates
ok, docs_claim_check ok, pyflakes rc 0, bandit rc 0. C sources unchanged
this round (Python + docs only).

## 2026-10-31 (round 14, merge follow-up) — macOS sanitizer leg: LeakSanitizer is Linux-only

PR #73 merged (`d1729ba`). The main-tier CI run failed exactly one step:
`build-test (macos-latest)` → "Sanitizers (ASan+UBSan) over the whole test
suite", exit 134 (SIGABRT). Cause: the round-13 `detect_leaks=1` (Qodo #6)
- AppleClang ships no LeakSanitizer, and the ASan runtime aborts at
startup when detect_leaks is requested on macOS. The ubuntu leg of the
same step was green, which masked the platform-specific option.

Fix (PR #75, merged `5ff3011`): the sanitizer step selects the option per
platform (`uname -s`) - `detect_leaks=1` on Linux, empty on macOS; ASan +
UBSan (`halt_on_error=1`) stay fully active on both legs. Verified:
workflow gates green; after the merge all three main workflows succeeded
(9/9 CI jobs incl. `build-test (macos-latest)`). `.agents/errors.md` +
`.agents/session.md` round-14 entries. Also removed an orphaned
preamble block that had sat mid-file since an earlier round.

## 2026-10-30 (round 13, Qodo/CodeQL audit of PR #73 @ c5fcd38) — engine hardening: all 26 open, fixable findings fixed with regression tests (+ the CodeQL alert)

Owner directive 2026-10-30 (verbatim intent): review every Qodo
finding + the CodeQL alert of PR #73 and verify whether each is really
fixed or still open, then fix what is open. Audit result: all 29
still-open findings + the CodeQL alert CONFIRMED (0 false positives,
0 already fixed by a later commit). Outcome of the 30: 26 Qodo
findings fixed in this round (23 code + 3 docs) + the 1 CodeQL alert
fixed (code); the 1 already-resolved finding confirmed; 3 explicit
deferrals with the reason.

1. **Converter (5 fixes, 38-check gate).** File-order tensor copy
   (the v1/v2 layout bug), expert up/gate scaling (gate rows x E,
   up rows x E — `masked_block`), strict token normalization
   (general `normalize_token`), U16 merge-operand overflow refusal,
   norm-shape refusal. Gate: `gguf_to_hydra2_test.py` 26 -> 35 checks (the "38" written at the time was a miscount - the file at commit 3c448c9 runs 35 checks, re-verified 2026-10-31).
2. **MoE engine (9 fixes).** Ternary in-place dequant aliasing
   (separate packed buffer + public `hydra_moe_dequant_flat`, AT64
   stream re-pinned — the pre-fix stream was silently wrong);
   sampler temperature now scales the EXPONENT (`exp((v-mx)/T)`;
   pre-fix T cancelled out entirely, pinned by a 3-temperature test);
   repetition penalty walks the whole window (pre-fix capped at the
   vocab size); stochastic path requires a non-NULL RNG; UTF-8
   validator distinguishes "invalid at offset 0"; 64-bit shape
   products + `n_heads % n_kv_heads == 0` refusal; GQA BLOCK mapping
   (head i reads KV head `i/(nh/nkv)`, llama.cpp convention — the
   pre-fix interleave `i % nkv` is wrong for nkv > 1, new GQA
   fixture + stream pin); mismatched KV-cache dim refused; trunk
   mapping munmapped with the REMEMBERED length (pre-fix
   `munmap(ptr, 0)` leaked it — CI LeakSanitizer now runs ENABLED
   on all five binaries, all clean).
3. **CLI (5 fixes).** Repetition ring is a true circular buffer
   (pre-fix: every post-fill write hit slot 0; `--rep-window 0` was a
   SIGFPE — both pinned in `text_prompt_check.sh`); positional
   defaults/clamp apply to `.hydra2` too (pre-fix a bare
   `hydra-run model.hydra2` ran with start=-1/steps=-1); the prompt
   now CONDITIONS the model (prefill + continue from the last prompt
   token — pre-fix the generated stream ignored the prompt
   entirely); generated stream printed as decoded text (`[Text-out]:`
   + JSON `generated_text`); `--inspect` JSON emits complete JSON
   values (no more double-quoted hex blobs) and a failed read no
   longer echoes the path.
4. **Server (1 fix).** `/api/models/inspect` uses ASYNC `execFile`
   (the pre-fix `execFileSync` stalled the whole event loop for up to
   the 10 s timeout; the async error carries the exit code in `.code`,
   probed alongside `.status`).
5. **CI (1 fix).** Sanitizer gate: `detect_leaks=1` (verified clean
   on all five binaries in the local sandbox before enabling) + the
   7th MoE fixture (GQA) in the suite invocation.
6. **Docs.** `.agents/session.md` rounds 6-11 archived verbatim to
   `.agents/docs/SESSION-ARCHIVE.md` (the archive rule); `CHANGELOG`
   "local" verification lines now name the machine; FORMAT-V2.md
   updated (GQA block mapping, 64-bit shape checks, KV-dim refusal,
   sampler contract, `trunk_map_len`, inspect JSON contract).

Deferrals (recorded, not silent): JNI batch test for the `.hydra2`
MoE engine (separate milestone — the JNI bridge has v3/v4 coverage),
SIMD dispatch for the MoE matvecs (scalar is correct; a benchmark
round decides), zero-copy expert streaming (the packed-buffer
contract of fix 2 makes it a format-level decision, not a patch).

Verification (local sandbox 2026-10-30: Linux e2b.local 6.1.158+
x86_64, gcc 12.2.0, Python 3.11.2): `make test-run` -> hydra-test
234/0 (x86 baseline 234), jni-batch 128/0, large-model 7/0, hydra2
152/0, **hydra-moe 180/0** (138 -> 180: the dequant-aliasing unit,
the shape/GQA + wrap-tamper section incl. the new GQA fixture, the
temperature / whole-window / NULL-RNG sampler pins, the UTF-8
offset-0 refusal; the ring + defaults CLI pins live in
text_prompt_check.sh), gguf 101/0, converter 38/0;
`text_prompt_check.sh` ok (parity + ring pin + defaults pin);
`server_test.js` 47/0; ASan+UBSan **with detect_leaks=1** all five
binaries 0 errors / 0 leaks; gcc -fanalyzer rc 0 on all four
translation units (26 pre-existing false-positive notes in
hydra_moe.c, down from 40 before the fixes); workflow YAML/lint
gates ok; test-count baseline 234 unchanged (main suite untouched).

## 2026-10-27..30 (round 12, issue #72) — the `.hydra2` MoE inference engine, block-scaled ternary, the BPE vocabulary path, and the GGUF -> `.hydra2` converter (M1-M5 full scope)

Owner directive 2026-10-27: "alles fixen was geht!!!!" (fix everything that can be fixed) — every
actionable item of issue #72 in one branch/PR (audit with per-claim
file:line evidence: `.agents/docs/ISSUE-72-AUDIT.md`).

1. **JNI v3/v4 batch gap (M1).** `tests/test_jni_batch.c` 109 -> **128**:
   deterministic v3/v4 writers, batched-run == direct-run stream
   equality for greedy AND fixed-seed sampling through the same C
   logic the bridge calls, the v4 stream pin, the token-0 fixed point
   of the bias-free design, the 17-step partial block with a real step
   function.
2. **Trunk mmap (M3.2).** `src/hydra_v2_container.c` maps the resident
   trunk region when possible (page-aligned extent + base offset);
   tensor reads serve from the mapping, `pread` stays the documented
   portable fallback; `trunk_backend` names the live path. The
   hydra2 suite grew to **152** checks (both backends pinned).
3. **Block-scaled ternary (M2.2).** New types `0x0040` TERNARY2_S64 /
   `0x0041` TERNARY2_S128 (2-bit codes + per-group F32 scales, paired
   `<name>.scale` tensor, `ceil(n/G)` scale count — FORMAT-V2.md 3.2.1);
   `hydra_tf_row_dot_scaled` (integer-exact group partials * F32 FMA,
   group-64 SIMD / group-128 scalar split); +9 main-suite checks
   (x86 baseline 234 / arm 243 in `tools/test_count_baseline.txt`).
4. **MoE inference engine (M3.1).** NEW `src/hydra_moe.c` +
   `include/hydra_moe.h`: `.hydra2` `moe-transformer-v1` models run in
   `hydra-run` — per-layer GQA (F32 attention, partial RoPE from
   `v2.rope_theta`, per-layer sink+ring KV), deterministic top-k
   routing, one-pread-per-expert FFN (F32 or ternary+scales layout),
   real per-vocabulary logits, min-p/rep sampler. Documented
   deviations: non-causal attention over cached positions, fixed 1e-6
   RMSNorm epsilon, bounded KV (2+8). NEW `tests/test_hydra_moe.c`:
   **138 checks** (parity vs independent reference, greedy stream pins
   per layout — incl. the attention-ternary fixture — KV pins, ternary
   dequant unit vs independent reference, tamper + metadata refusals).
   `main.c` magic-dispatches `.hydra2` to the engine; the v1 path is
   bit-identical (pinned streams re-verified).
5. **Vocabulary + BPE + web inspection (M4).** Role-8 tensors
   (`vocab` 0x0050 VOCAB_UTF8 / `merges` 0x0051 U16_PAIRS, FORMAT-V2.md
   3.2.3), strict codec in the engine (byte-level tokens, loud
   refusals: duplicates, out-of-range ids, missing concat target,
   missing trailing NUL), `hydra-run --text-prompt` (BPE-encodes user
   text with the embedded vocab; parity with numeric `--prompt`
   pinned; JSON "text" echo; mutual exclusion; refusals), web console
   `.hydra2` inspection (`/api/models/inspect`, `public/app.js`;
   `server_test.js` 47 checks), `make_model.py --moe-full`
   vocab/merges fixtures. Gate: `tools/text_prompt_check.sh` (CI gate
   1h).
6. **GGUF -> .hydra2 converter (M2.2/M3.1/M4).** NEW
   `tools/gguf_to_hydra2.py`: a decoder-only llama GGUF (official AND
   pinned-build tensor names; F32/F16/BF16/Q8_0) becomes a
   complete-contract `.hydra2` MoE model — linears transposed, dense
   FFN decomposed into E full-width masked experts (gate/up rows
   partitioned and x E, down full, all-zero uniform router -> exact up
   to F32 order), embedded vocab 1:1, merges filtered to expressible
   rules (skips documented), rope_theta/bos carried; loud refusals for
   non-GGUF / non-llama / undecodable tensors / missing tokens / dim
   mismatch. NEW gate `tools/gguf_to_hydra2_test.py` (26 checks:
   engine determinism, expert-CRC tamper refusal, F32-exact embed
   transpose + masked-expert identity + byte-exact vocab + metadata,
   merges roundtrip, five refusal controls); Makefile
   `gguf-conv-test` in `test-run`; CI gate 1i. Docs: GGUF-IMPORT.md
   12, FORMAT-V2.md 3.2.1/3.2.3 + implementation status.
7. **Sanitizers in CI (found a real UB).** An ASan+UBSan rebuild
   (same sources) flagged a misaligned float load in the ternary
   expert scale read (rows*cols % 4 == 2): fixed with a memcpy read in
   `src/hydra_moe.c` + the test reference (bit-identical values); NEW
   CI step "Sanitizers (ASan+UBSan) over the whole test suite" runs
   all five test binaries with `UBSAN_OPTIONS=halt_on_error=1`.
   `errors.md` entry. Also fixed: the lint job never built `./hydra-run`
   although gate 1h drives it — a build step now precedes the
   binary-driven gates.

Verification (local sandbox 2026-10-30: Linux e2b.local 6.1.158+
x86_64, gcc 12.2.0, Python 3.11.2): `make clean && make && make test`
green; `make test-run`: hydra-test 234/0 (x86 baseline), jni-batch
128/0, large-model 7/0, gguf 101/0 (v1 gate untouched), gguf-conv
26/0, hydra2-test 152/0, hydra-moe-test 138/0;
`node --test tools/server_test.js` 47/0; ASan+UBSan all five binaries
0 runtime errors; gcc -fanalyzer clean on all translation units;
cppcheck 2.13.2 exact CI command NOT runnable in this sandbox (the
repo-proxy whitelist stopped serving cppcheck/cppcheck and the sandbox
has no root) — covered by the CI lint gate; all shell gates green.

## 2026-10-26 (round 11) — post-merge audit: sampler contract across all surfaces, .hydra2 Android validation, and the tf_row_dot SIMD completion (M1–M5)

1. **One sampler validation contract across C, JNI and the web
   console (M1).** `android/app/src/main/cpp/hydra_sample_cfg.h` is
   now the single source for the sampler ranges (temp 0..20, min_p
   0..1, rep-penalty >= 1.0, rep-window 0..4096), the validation
   order (temp → min_p → rep; first invalid wins), the rejection
   codes and the greedy defaults (0/0/1.0/64). `hydra_jni.c` calls
   it (with a LOGE + JSON error payload); the CLI and `server.js`
   keep their own (documented) stricter/sparser variants. New gate
   `tools/sampler_contract_check.sh` (34 checks) cross-checks C
   header vs JNI header vs server.js constants, including a drift
   negative control ("C header '25.0' vs server.js '20'");
   `tests/test_jni_batch.c` gained 23 checks (boundaries, 14
   rejections incl. NaN/±Inf/first-invalid-wins, pinned defaults):
   86 → **109**.
2. **The web console speaks sampler (M2).** `server.js` validates
   all four sampler fields strictly (`typeof v !== 'number' ||
   !Number.isFinite(v)`; the window must additionally be an integer)
   and appends `--temp/--min-p/--rep-penalty/--rep-window` to the
   `hydra-run` invocation **only when the field is present** — a
   field-less request is bit-identical to the pre-sampler CLI. The
   console got a sampler section (`public/index.html` +
   `public/app.js`, which sends only non-default values). This
   caught a real server bug: `Number(null) === 0` had let a
   `NaN`-serialized field pass as temperature 0 — now 400.
   `tools/server_test.js`: 37 → **43** (bit-parity
   legacy==temp-0, the deterministic stream does change when
   temp/min_p are set, reproducibility, all four flags, boundaries,
   the 400 bad-value suite). `tools/hydra_train.js` deliberately
   untouched (deterministic hill-climbing, no sampling).
3. **The Android header check validates .hydra2 next to .hydra
   (M3).** `MainActivity.kt` now carries
   `HYDRA2_TAIL_U32 = 0x00003241L` (the little-endian u32 of
   `HYDRA2_MAGIC_STR` bytes 4..7) and a dedicated
   `HeaderRule("format", …)` with `err_magic_hydra2` in
   `strings.xml` — a `.hydra2` file is named as such instead of
   failing the generic version rule. `tools/android_header_check.sh`
   gained 7 checks incl. two **computed cross-surface** pins
   (python3 recomputes the Kotlin constants from the C magic
   strings), 21 → **28**; the negative control (a wrong tail)
   fails as required. The Kotlin stub check
   (`tools/kotlin_stub_check`) got the matching `R_gen.kt` stub and
   stays green (kotlinc via the Environment.md recipe).
4. **The v3/v4 transformer hot path is now SIMD on x86 and ARM
   (M4).** `tf_row_dot` (384 calls/token at dim 64) dispatches to a
   new **AVX2 kernel** (`src/hydra_simd.h`: 8 dims/chunk from 2 row
   bytes, nibble split, two 16-entry `pshufb` code tables, a fixed
   permutation, T3 through `pshufb`, `madd_epi16`) and a new
   **NEON kernel** (`src/hydra_neon.h`: same chunking, per-byte
   `vshr_n_u8` extraction without lookup tables, T3 from `vceq_u8`
   masks, `vmovl_s8` + two `vmlal_s16`), with the scalar loop as
   fallback. Both are **integer-exact** (lane-bound 2·127, total
   bound 64·127 = 8128), so AVX2/NEON builds produce bit-identical
   token streams — the pinned v3/v4 reference streams stayed valid
   without re-pinning (the two pinned stream tests are exactly what
   caught the first kernel draft). New parity tests
   (`test_avx2_tf_row_dot`, `test_neon_tf_row_dot`): independent
   scalar reference, full int8 range, x buffer allocated **exactly
   dim bytes** (the engine's state layout — this is what caught the
   two out-of-range loads, verified under ASan), 4-code table sweep
   at ±dim·127: x86 219 → **225**, ARM 228 → **234**
   (baseline updated in the same commit). The float parts of v3/v4
   and the container's top-k routing stay scalar by design
   (documented scope, `AGENTS.md` §2.1).
5. **CI wiring + docs (M5).** The `jni-signatures` job gained the
   two new gate steps (sampler contract + JNI signature negative
   control, SKIP-safe without javac); `tools/workflow_yaml_check.sh`
   re-validated all 5 workflows. `AGENTS.md` §2.1 now documents the
   v3/v4 format, the SIMD dispatch and the documented-scope
   decisions; §3 carries the new counts (jni-batch 109,
   server_test 43) and the two new gates. `README.md` documents the
   CLI sampler flags and their contract. The v1 starter baseline
   `[471 386 385 386 385 386 385 386]` is re-verified
   byte-identical.

## 2026-10-26 (round 10) — the documented smoke recipe bug, the all-.md English pass, real-model verification, the opt-in real-model gate

1. **The documented §11.4 smoke recipe never ran llama-cli (Qodo
   issue, confirmed and fixed).** The line was
   `timeout 10 OMP_NUM_THREADS=1 $LLAMA/…/llama-cli …`: `timeout`
   executes its first non-option argument *as the command*, so it
   tried to run the literal string `OMP_NUM_THREADS=1` (reproduced:
   `timeout: failed to run command 'OMP_NUM_THREADS=1'`). It is now
   `timeout 10 env OMP_NUM_THREADS=1 …`, and the fixed recipe was
   executed verbatim from a clean cwd (fixture output
   `1'aV+uE~`, prompt 34 410.1 t/s, generation 6 323.4 t/s).
2. **All artifacts translated to English (owner directive).**
   `.github/workflows/lint.yml` (6 comment blocks), `release.yml`
   (15), the `Makefile` (1 line), and
   `.agents/docs/SESSION-ARCHIVE.md` (rounds 3-6 + the doc-audit
   section, ~90 lines of German prose). Deliberately kept: verbatim
   owner directives quoted in `user.md`/`session.md`/`errors.md`, the
   MIT licence string, and the v1 product's German CLI output
   (product identity, quoted by the docs; the string-format gates
   enforce it). All workflow YAML re-validated.
3. **A real model was downloaded and run through the pinned
   llama.cpp** — the same v0.5.0 source the app's NDK build compiles
   into `libllama_jni.so`. **llama-68m-chat-v1, Q8_0, 73 018 688 B
   (73.0 MB)**, arch `llama`, 2 layers, dim 768, ctx 2048, 32k vocab,
   21 tensors (16 Q8_0 + 5 F32), from `BodhiSearch/BodhiApp` (HF-cache
   layout, plain git blob) via the codeload tarball. The pinned CLI
   (open stdin) ran it: "The capital of France is" →
   "The capital of France is France.<|im_end|>", prompt 109.4 t/s,
   generation 6.6 t/s (2 threads); a longer greedy prompt 172.7 /
   4.4 t/s; `--temp 0.7` generation 8.2 t/s. This is the **largest
   real writing model this sandbox's network can deliver**: the
   sandbox only reaches github/codeload/api/npm/pypi, and GitHub
   stores model files ≥100 MB via LFS (the LFS endpoint is
   unreachable) — proven for gemma-3-270m-it-Q8_0 (291.5 MB),
   qwen2.5-0.5b-instruct-q4_k_m and smollm2-135m (every copy an
   LFS pointer). The 11.7 recipe works unchanged for a ~300 MB model
   on a normal machine.
4. **The app path is proven with that model.** `LlamaBridge.load`
   → JNI → `llama_model_load_from_file` (pinned v0.5.0) →
   generate/cancel/unload is a thin wrapper with no size/arch/quant
   restriction of its own; architecture is delegated to the pinned
   loader (the README claim, now backed by a real model). The
   model's ctx 2048 fits the app's clamp 256..8192. Not measured
   (R32, no device): the `.so` inside the APK on a phone/emulator —
   the Android CI tier compiles the exact JNI source; the x86_64
   emulator is the documented device-side path.
5. **The opt-in real-model gate** — `tools/gguf_test.py --real-model
   <path>` (6 checks, off by default so CI stays offline and fast):
   the GGUF magic (bytes `47 47 55 46`), a full parse, every
   decodable tensor decodes, the converter projects the model to a
   `.hydra` v3 file (dim 64, 2 layers, vocab 1024, 30 976 tokens
   dropped — the honest projection), and the C engine runs that
   projection deterministically (two runs byte-identical, non-
   degenerate). On llama-68m: **107 checks, 0 failed**; the default
   offline set stays 101.
6. **Docs** — `docs/GGUF-IMPORT.md` 11.7 (source, download + gate
   recipe, measured numbers, what each tool proved, the size ceiling
   and what is not measured), `README.md` (GGUF bullet + tools tree),
   `.agents/{session,errors,Environment,user}.md` (round 10, the two
   new mistakes — the `timeout` env-var trap and the GitHub LFS
   API ambiguity — the model location + re-fetch recipe, directive 29).

## 2026-10-26 (round 9) — GGUF verification: the quant type table bug, the pinned llama.cpp smoke test, the converted-model behavior gate, the `.hydra2` GGUF boundary

Answer to "can GGUF models really be used, with their full
intelligence?": the app is dual-engine. The **llama.cpp engine**
(submodule pinned at v0.5.0, `7fe450e19305b828c199d602c23a8337aaa1f03b`)
is the main engine and runs GGUF with full transformer inference; the
`GGUF → .hydra` converter is an honest projection (seed model, not the
source network). This round verified both paths with tests and
recorded everything in `docs/GGUF-IMPORT.md` §11.

1. **The quantised-type name table was wrong (bug found by audit
   against the official `ggml_type` enum, ggml-org/ggml,
   `include/ggml.h`).** `tools/gguf_reader.py` said `31 = TQ1_0`,
   `32 = TQ2_0`, `34 = MXFP4`; officially `TQ1_0 = 34`, `TQ2_0 = 35`,
   `MXFP4 = 39`, with `NVFP4 = 40`, `Q1_0 = 41`, `Q2_0 = 42` missing
   and slots `31–33`/`36–38` removed/reserved. A TQ1_0 file would have
   been refused under the label `MXFP4`. Fixed; the converter's
   refusal now names tensor **and** layout (`token_embd.weight
   (TQ1_0)`). Regression: `run_type_names` in `tools/gguf_test.py`
   (all six current names + all six reserved slots + the converter
   message).
2. **GGUF spec conformance verified** against ggml-org/ggml
   `docs/gguf.md` (fetched 2026-10-26): header order, all 13 metadata
   value types, tensor info, offset-relative-to-data-section,
   alignment; reader accepts version 2–3. LE-only is spec-conformant
   (the spec says to assume little-endian; it cannot be detected);
   the 64-byte tensor-name cap is not enforced (harmless here) — both
   now documented.
3. **The converted model is gated on what it actually writes**
   (`run_behavior` in `tools/gguf_test.py`): deterministic (two
   independent `./hydra-run` runs byte-identical) and non-degenerate
   (≥2 distinct tokens, not the input repeated) for three
   (token, steps) pairs. The honest ceiling of a projection — stated
   in the gate and in the docs.
4. **The pinned llama.cpp smoke test** (`tools/gguf_minimal_llama.py`,
   stdlib-only, fixed seed, structurally real 2-layer 64-dim llama
   GGUF: full per-layer tensor set, 99-token BPE charmap, empty
   merges). Result (2026-10-26, this box, 2 cores): the pinned v0.5.0
   build loads the generated file and runs the full pipeline —
   prompt 36 082.5 t/s, generation 5 640.6 t/s, generated text
   `1'aV+uE~` (random by construction, byte-identical across runs).
   Generator fixes baked in from the run: `tokenizer.ggml.model` =
   `"gpt2"` (not `"BPE"`), `tokenizer.ggml.token_type` INT32 with the
   pinned enum (chars NORMAL=1, specials CONTROL=3), the add-bos and
   padding keys under `tokenizer.ggml.*` (not `llama.*`),
   `output_norm.weight` present, `llama.feed_forward_length` set,
   `ffn_gate/up` = `{n_embd, n_ff}` with `ffn_down` =
   `{n_ff, n_embd}` in this build. `run_minimal_llama` keeps the
   generator alive in CI (our own reader must parse the file, with
   the F32 dtype, token types and keys); the llama.cpp step itself is
   manual (CI does not build llama.cpp) and documented in
   `docs/GGUF-IMPORT.md` §11.4.
5. **The `.hydra2` container boundary** (`src/hydra_v2_container.c`):
   a GGUF file dropped into the v2 reader is now refused with the
   GGUF magic **named** (bytes `0x47 0x47 0x55 0x46`, LE u32
   `0x46554747`) and a pointer to
   the two supported GGUF destinations. `+2` checks in
   `tests/test_hydra2_container.c` (suite 120 -> 122;
   `tools/test_count_baseline.txt` updated in this commit).
6. **Docs.** `docs/GGUF-IMPORT.md` §11 (verification round), §4
   (refusal example now names tensor + type), §9 (container boundary,
   LE-only); `README.md` (dual-engine Android feature, GGUF import
   bullet, tools tree, 122 checks); `docs/FORMAT-V2.md` (GGUF not a
   v2 container); `.agents/session.md` + `.agents/errors.md` +
   `.agents/Environment.md` (round 9, the new mistakes, the
   llama.cpp build environment).
7. **Qodo code-review fixes on PR #68** (all 11 findings, each
   verified against the pinned sources before acting): the
   `tools/session_start_check.sh` R33 marker for the round 9 section
   was missing (would have failed in CI); the two tokenizer keys were
   renamed to the `tokenizer.ggml.*` namespace the pin actually reads
   (llama-arch.cpp — the `llama.*` names are parsed but ignored);
   the README now states the llama.cpp engine's availability per ABI
   (arm64-v8a selectable, x86_64 packaged but not selectable,
   armeabi-v7a absent); `run_behavior` now asserts the exact
   requested token count; the fixture's token types were reversed
   (UNKNOWN chars are dropped by the BPE detokenizer — the root cause
   of the first empty smoke output) and are now NORMAL/CONTROL;
   the dtype defaults are aligned on f32 (library, gate, docs); the
   §11.4 intro describes the fixture as F32; the LCG produces real
   (-1,1) weights; the GGUF magic in the v2 refusal message (and
   FORMAT-V2, this file) is written as the byte sequence 0x47 0x47
   0x55 0x46 / LE u32 0x46554747 (the message had the big-endian
   value); the generator docstring no longer carries stale numbers;
   the §11.4 recipe runs the generator from the repository root.
   Smoke re-run: prompt 36 082.5 t/s, generation 5 640.6 t/s,
   `1'aV+uE~` (deterministic). Deliberately NOT added (owner call,
   standing "CI speed up safely" directive): a CI job that builds the
   pin and runs the smoke automatically — Qodo's optional suggestion,
   10-15 min per PR.

## 2026-10-26 (round 8) — JNI sampler exposure (M-E), format version 4 full attention (M-F), the `.hydra2` sparse-MoE container implemented (M-G), baselines + checkboxes (M-H)

All changes this round are ADDITIVE: the v1 starter baseline stream
(`471 386 385 386 385 386 385 386`, start token 42) was re-verified
byte-identical, and no v1/v2/v3 file or code path was rewritten.

1. **The C sampler is exposed to Android (M-E).** `hydra_jni.c` gains
   `hydra_set_sampling(temperature, min_p, repetition_penalty)` /
   `hydra_sample_token()` / `hydra_get_last_logit_max()` on top of the
   existing engine API; `HydraBridge.kt` + `HydraEngine.kt` wire them
   through (Kotlin stubs updated). The default path (greedy via
   `hydra_engine_step`) is bit-identical to before — proven by tests.
   C-side: `+12` checks in the main suite (parameter plumbing,
   greedy-unchanged proof, negative controls).
2. **Format version 4: full per-layer transformer attention (M-F).**
   New version value in the `HYDR` header (v1/v2/v3 files untouched):
   per-layer Wq/Wk/Wv ternary planes in addition to the v3 Wg/Wu/w1/
   scale. `hydra_transformer_forward_step()` = pre-norm RMSNorm ->
   ternary Q/K/V (NEON/AVX2/scalar dispatch) -> RoPE -> KV cache
   (sinks + window) -> causal scaled dot-product attention
   (deterministic fixed-point) -> residual -> RMSNorm -> SwiGLU FFN ->
   residual -> final RMSNorm -> W_out logits. CLI + engine dispatch.
   C-side: `+33` checks (loader validation, KV progression, RoPE exact
   key rotation vs the independent float reference, hand-computed
   forward pass, min-p forced case, baseline-stream-untouched guards).
   Test baseline: x86 174 -> 219, arm 183 -> 228
   (`tools/test_count_baseline.txt`, same commit, per-file convention).
3. **The `.hydra2` container is implemented (M-G) — the spec
   `docs/FORMAT-V2.md` gained a real reader.** New
   `src/hydra_v2_container.c` / `include/hydra_v2_container.h`:
   128-byte header parser with header CRC-32 verified at open;
   BOTH-ENDS validation of every region/entry per section 5 of the
   spec (bounds, alignment 16 B, sizes, counts, per-element type
   rules, order + contiguity of the expert directory); metadata K/V
   block (wire format now documented in the spec as a `[CODE]`
   decision: `u32 count` then per pair `u32 key_len, u32 val_len,
   key, value`); `hydra2_read_tensor()` (trunk),
   `hydra2_read_expert()` (one `pread` per expert, CRC-32 verified at
   read time per spec 5.1.5), `hydra2_route_top_k()` (deterministic
   top-k, ties to lower id, softmax over selected logits). `tools/
   make_model.py --v2` writes the fixture format; the reader and the
   writer are cross-checked in tests. New test binary
   `tests/test_hydra2_container.c` (`make hydra2-test`, 122 checks,
   run + failure-gated in CI, fixture auto-generated by
   `tools/make_model.py --v2` in the CI step): happy path, every
   refusal class (bad magic/version/CRC/counts/alignment/order/
   trailing bytes/non-ASCII keys, both-ends overflow on every read
   path), routing determinism, Python/C cross-check, mutual v1/v2
   refusal, missing/symlink open errors. The v1 loader now checks the
   8-byte v2 magic before the v1 magic (first four bytes of
   `HYDRA2\0\0` are `HYDR`) and refuses v2 files loudly per spec 7.2.
   What does NOT exist (stated in the spec): MoE inference on top of
   the container, the expert cache (spec 6.2), prefetch (6.4), and
   every section-6 throughput number remains NOT MEASURED.
4. **Static-analysis gate (M-G + owner directive).** CI `lint.yml`
   now runs `gcc -fanalyzer` (C99, `-Wall -Wextra`, CI-identical
   flags) on all three C files plus `clang --analyze` / `clang-tidy`
   where available; the analyzer caught and fixed two real bugs in
   the new container reader (a partial-state leak on a metadata
   parse failure and a dereference-after-free in a refusal message).
   Local: `gcc -fanalyzer` clean on all three C files;
   `hydra2-test` clean under ASan+UBSan (zero leaks).
5. **`docs/FORMAT-V2.md` updated to the truth (M-H/M-I):** the
   "nothing is implemented" banner is replaced by an exact
   implemented/not-implemented boundary; the K/V wire format and the
   16-byte alignment are recorded as dated `[CODE]` implementation
   decisions; the type table notes which types are validated vs
   decoded. `docs/claim_check` (the banned-claims gate) still passes.
6. **Milestone checkboxes (M-H, M6 invariant).** `.agents/session.md`
   marks M-A..M-H `[x]` — each box was checked only after its
   deliverable was verified present in the tree and covered by
   automated tests.

## 2026-10-26 (round 7) — rule adaptation (R33 + gate 1j), canonical Agent Skills in `.agents/skills/`, session.md slimming, all-.md audit

1. **The read-before-work rule is now enforced, not prose.** Rule **R33**
   (`.agents/rules.md`): at session start the agent reads
   `AGENTS.md` → the current round of `.agents/session.md` →
   `.agents/errors.md` (top entries) → `rules.md` → `user.md` →
   `Environment.md` (own section) → the task-relevant skills, and writes
   a verifiable marker `**Session start (R33):** <date> (branch …).
   Read: <files>.` into the current round section BEFORE the first code
   edit. New CI **gate 1j** (`tools/session_start_check.sh`, wired into
   `ci.yml` after 1i) fails the build when the current round section
   lacks the marker; the detector is proven by a negative-control
   fixture inside the gate (R25). Basis: 2026 context-engineering
   research — prose-only rules are followed probabilistically ("only
   enforcement guarantees it"), and compliance odds drop ~5.6% per
   additional function in the instruction set (arXiv May-2026 study of
   1650 sessions).
2. **`.agents/skills/` is now the canonical skills home** (2026 Agent
   Skills spec: `name` == directory, quoted `description` with what +
   when, body < 5000 tokens). The 3 skills that existed in
   `.claude/skills/` moved there via `git mv`
   (`session-workflow`, `engine-ci-verify`, `android-ndk-build`) and 4
   new ones were added, written from verified repo knowledge only:
   `ci-triage` (check-run annotations, the zero-job compile-failure
   signature, local gate replicas), `kotlin-stub-check` (setup + the
   stub-fidelity rules learned the hard way), `android-lint-gate`
   (reading the lint job's annotations, the lint-only classes
   ResourceType / PluralsCandidate / AutoboxingStateCreation /
   UnusedResources and the correct APIs), `repo-docs-audit` (the
   repeatable audit of every Markdown file). `.claude/skills` is now a
   relative symlink to `../.agents/skills` so tool auto-discovery keeps
   working while all agent docs stay in `.agents/` (R30).
3. **`session.md` slimmed for the context budget:** every round before
   round 6 moved verbatim to `.agents/docs/SESSION-ARCHIVE.md`
   (append-only, nothing deleted); the live file now holds the summary
   tables + the open rounds (~385 lines instead of ~2490).
4. **All-Markdown audit (every folder):** stale PR states, a dead
   reference to the deleted `.github/workflows/code-scanning.yml` in
   `AGENTS.md` §8 (rewritten to the verified reality: no
   alert-handling workflow can exist — `code_scanning_alert` is a
   webhook event, not a workflow trigger event), broken cross-links
   `docs/FORMAT-V2.md` ↔ `.agents/docs/HYDRA2-RESEARCH.md`, the root
   `README.md` repo tree, `.agents/README.md` index, the
   `session-workflow` skill's format template, and a
   `RESEARCH-LOG.md` §7 correction (the event-triggered workflow shipped
   that morning could never have run; deleted the same evening — native
   alert closure stands). Superseded PR #62 (red leftover of the #59
   merge incident; its red = the stage-2 API error fixed by merged PR
   #64) closed as superseded.

## 2026-10-26 (round 6 follow-up) — stage-2 plurals fix, phantom-run root cause, gate 1i (PR #64, main 19df869)

1. **Stage-2 plurals fix:** `MainActivity.kt` line 658 called
   `context.getQuantityString(…)` — `Unresolved reference` on CI's
   Kotlin compile (the method is on **`Resources`**, not `Context`;
   verified against the official API reference:
   `Resources.getQuantityString(int, int, Object…)` since API 1).
   Fixed to `resources.getQuantityString(R.plurals.log_context_applied,
   new.contextSize, new.contextSize)` — the pattern the same file
   already used at three other call sites. The stage-1 attempt had
   papered over the same error by **inventing** a `Context.
   getQuantityString` member in the Kotlin stub — removed; stubs may
   only carry real API surface (`tools/kotlin_stub_check/`, errors.md).
2. **Root cause of ALL zero-job phantom runs:** `on: code_scanning_alert`
   is a Webhook event, **not** a workflow trigger event — GitHub's
   workflow compiler rejects any file naming it as a trigger: the run is
   created with zero jobs, fails in the same second, and the run's name
   is the file path (the "corrupted registration" theory was wrong).
   The alert-handling workflow was deleted; alert closure is native
   GitHub behaviour (a CodeQL/Semgrep finding resolves as *fixed* when
   the next default-branch analysis no longer reports it).
3. **Gate 1i (new):** `tools/workflow_lint_check.sh` runs actionlint
   over every workflow file (npm WASM, /tmp, self-recreating) with an
   in-gate negative-control fixture (R25) — it fails on invalid trigger
   events, which PyYAML (gate 1h, duplicate keys) structurally cannot
   see. PR #64 CI: 15 pass / 4 skipped / 0 fail.

## 2026-10-26 (round 6) — CI red fix, local Kotlin type-check, `.agents/` reorg

1. **Five red-CI root causes fixed** (all four workflows on main were
   red): (a) cppcheck `uninitvar` ×2 in the v3 roundtrip test (buffer
   now zero-initialized per codebase convention); (b) `HydraApp.kt`
   `SettingSlider` used `.last`/`.first` on
   `ClosedFloatingPointRange` (IntRange-only members) — the slider now
   takes `start: Float, end: Float` + step, 6 call sites converted;
   (c) the zero-job phantom runs of the code-scanning workflow (true
   root cause and fix: see the round-6-follow-up entry above); (d)
   clang gate 1b `-Werror,-Wtypedef-redefinition` on
   `hydra_kv_cache_t` (C99: the typedef was declared twice; the full
   definition is now a plain `struct`, name kept by the forward
   declaration) — GCC was silent (the diagnosis sits behind
   `-Wpedantic`), so it needed the new local `zig cc` Clang replica to
   reproduce; (e) the Kotlin `weight` import "fix" was a FALSE ALARM —
   `weight` is a member extension of RowScope/ColumnScope, the import
   is rejected by K2; the import was removed and the stub corrected to
   mirror the real API (both directions recorded in errors.md).
2. **New local capability: Kotlin type-check without the Android SDK**
   — `tools/kotlin_stub_check/` (kotlinc 2.4.21 via npm + a 40-file
   stub classpath mirroring the exact android/androidx/compose/
   org.json API surface, R generated from the real
   `res/values/strings.xml`): `bash
   tools/kotlin_stub_check/kotlin_stub_check.sh` type-checks all 10 app
   .kt files in ~20s. This is the local equivalent of the CI Kotlin
   compile step and is what found root cause (b) locally. Stub
   fidelity rules: verify every signature against the real API first,
   one package per stub file, a stub error means *app OR stub* is
   wrong — the real API is the tie-breaker.
3. **`.agents/` reorg (owner directive):** all agent-specific docs
   moved into `.agents/` (`rules.md`, `errors.md`, `session.md`,
   `Environment.md`, `CLAUDE.md` + 8 docs; `.agent_memory/` dissolved
   into `docs/{TECHNICAL-AUDIT,RESEARCH-LOG}.md`); the root keeps
   `README.md` + `AGENTS.md`, `docs/` keeps the product specs. All
   path references updated repo-wide (44 files). New:
   `.agents/user.md` (owner's standing directives, dated) and
   `.agents/README.md` (index + maintenance rules).
4. **Verification (all local, real outputs):** `make test-run`
   174/0 + 86/0 + 7/0 + 73/0 unchanged; cppcheck `--enable=all` 0
   issues; stub check 0 errors; docs-claim gate clean; workflow-YAML
   gate (1h, duplicate keys) + PyYAML full parse of all 5 workflows;
   string-format check 39 ok; `zig cc` gate-1b replica 5/5 files clean
   (reproduced the red before the fix); NEON path full compile rc 0.

## 2026-10-26 (round 5) — UI/UX bug fixes + expanded Android settings

1. **UI/UX bugs (all with code evidence; the attached "RepoInspect" PDF
   was verified INVALID for this repo and ignored):** (a) the model
   row's "state 17 B" metric was the *serialized* llama state size at
   n_past=0 (misleading); the load report now carries `kv_bytes`
   (2·n_layer·n_ctx·n_embd·2B, F16 KV, per the pinned v0.5.0 headers)
   and `logits_bytes` (n_vocab·n_embd·4B); (b) the chat never
   auto-scrolled — `rememberLazyListState` + a `LaunchedEffect` keyed on
   (last message id, text length), following only while the user is at
   the bottom; (c) the keyboard stayed open after Send — now hidden via
   focus-clear + keyboard controller; (d) the landscape-with-keyboard
   layout collapse (composer at the top, chat invisible) was a REAL
   inset bug: `imePadding()` on the input row alone — the IME inset now
   applies to the ChatTab root column; (e) the telemetry row reserves a
   minimum height (no first-run jump).
2. **Expanded settings (the explicit ask):** a real SettingsTab
   (replacing "coming soon"): thinking/chain-of-thought switch,
   system-prompt field, temperature 0–2, top-k 1–100, min-p 0–0.5,
   repetition penalty 1–2, max tokens 32–1024, context size 512–8192
   (llama, needs reload — said so in the UI), reset-to-defaults.
   Wire-through: `EngineInterface.generate` gains a
   `GenerationParams`; `llama_jni.cpp` `build_sampler_chain` is
   parameterized (top_k, min_p, rep penalty — API confirmed in the
   pinned header) with the chain rebuilt on key change; the
   deterministic HydraEngine ignores sampling params (documented on the
   interface); system prompt + thinking prompt prepend for the text
   engine. All settings + engine choice + auto-run persist via
   SharedPreferences. A real Models tab (replacing the placeholder):
   current-model facts, import, and removal of an imported model
   (back to the bundled starter).
3. **Verification (honest split):** `g++ -fsyntax-only` of
   `llama_jni.cpp` against the PINNED llama v0.5.0 submodule headers +
   a minimal `jni.h` stub (no NDK locally — the stub limits the check
   to the llama-API surface); Kotlin stub check 0 errors; the full CI
   tier green in the PR (the round-6 entry documents the two lint
   findings CI then surfaced and their fixes).

## 2026-10-26 (round 4) — true per-layer execution (format version 3), full-dimension decode, converter v3 modes, 672-B pin

1. **The aggregated recurrence is now a legacy mode, not the
   architecture.** The loader accepts a new format **version 3**
   (same 24-byte header): per layer it stores ternary weight matrices
   `Wg`/`Wu` (dim×dim, 2-bit packed, 4 weights/byte), a token-injection
   `w1` plane, and a u16 LE per-layer absmean scale; after all layers a
   final `W_out` dim×dim block. `hydra_engine_step_tf()` executes a
   real forward pass per token, directly from the mmap (zero heap,
   stack-only working set): `xeff = clamp127(state + w1·token)` →
   ternary BitLinear mat-vecs `gate = Wg·xeff`, `up = Wu·xeff`
   (int32-exact add/sub, BitNet b1.58 W1.58A8 pattern: ternary weights
   × int8 ±127 activations, per-layer absmean rescale) → SwiGLU →
   RMSNorm → RoPE(`step_count`) → int8 state; once per token the KV
   cache's sink-0 residual is added and the final state is appended;
   then the **full-dimension** logit mix `y = W_out·state` is decoded
   (greedy `argmax mod vocab`; `hydra_sample_token()` v3 mode feeds the
   same vector through the existing repetition/softmax/min-p/seeded
   sampler). **The v1 column-0 bottleneck is gone in v3**: every weight
   in every dimension influences the output. `tests/test_engine.c`
   proves it two ways — a dim-`d>0` weight flip changes the v3 token
   stream (`test_tf_dim_positive_weight_changes_stream`) while the
   same-flipped v1 twin is bit-identical, and a 4-layer v3 model is NOT
   reproducible by any load-time sum of its planes
   (`test_tf_multilayer_not_reducible_by_loadtime_sums`).
2. **A real engine bug was found and fixed by the new tests.**
   `step_tf` read the per-layer u16 scale from the w1 plane's start
   instead of after it (`w1p[0..1]` vs `w1p + dim/4`), silently using
   sf=1/256. The POS hand-built model passed by accident (SiLU
   saturation + RMSNorm scale invariance); the NEG model's sigmoid
   underflowed to a dead state. Fixed with the correct offset plus a
   comment; the hand-built 2-bit decode tests (POS state +64, NEG state
   −64, hand-derived tokens) are the regression guard.
3. **The legacy path is isolated, not silent.** `hydra_engine_step()`
   and `hydra_engine_step_fast()` refuse v3 engines with `-14`,
   `hydra_engine_step_tf()` refuses v1/v2 with the same code; the CLI
   routes v3 models to the per-layer pass (bench + generation) and
   reports `--fast` as N/A on v3. Loader additions: v3 body-size
   validation (−10), per-layer scale-0 refusal (−15, walks the mapped
   scale words at load), non-finite forward pass (−16), KV append
   failure (−17).
4. **Both converters write v3.** `tools/gguf_to_hydra.py --v3
   [--v3-layers N]` and `tools/safetensors_to_hydra.py --v3
   [--v3-layers N]`: gate/up blocks are the ternarised top-left
   dim×dim slices of real candidate tensors (cycled deterministically,
   two distinct tensors per layer), `w1` from cycling embedding rows,
   `W_out` from the first tensor; the per-layer scale is the
   absmean(·256) of the source (max of gate/up), clamped to
   [1, 65535]. Each writer runs a **stdlib-only write-time roundtrip
   check** (re-read the file, re-decode the first scale + the whole
   `W_out` block, abort on mismatch). The JSON report names every
   source tensor, its γ, its density and any zero-padding — the
   converter states "dim×dim slice of a larger tensor" instead of
   pretending to be the full model. `tools/gguf_test.py` gained 14 v3
   gates (59 → 73): byte-level layout verification for both converters,
   non-zero scales, non-zero W_out, two-run engine determinism, and
   refusal of `--dim 10` / `--v3-layers 0`.
5. **`sizeof(HydraEngine)` 664 → 672 B (LP64).** The v3 RoPE position
   counter `step_count` (u32 + 4 B trailing padding) was added; the
   compile-time pin in `include/hydra_model.h` moved 664 → 672 and the
   Memory Model table in `docs/ARCHITECTURE.md` (and the README) now
   derive the 672 B field by field.
6. **Everything documented.** `docs/FORMAT.md` "Version 3" is the
   canonical spec (layout, field semantics, forward-pass contract,
   error codes); `docs/ARCHITECTURE.md` gained the "Version 3 forward
   pass" section and the v1 "algebraically free" section is scoped to
   v1/v2; new `.agents/docs/RESEARCH.md` records the web-verified BitNet b1.58
   (W1.58A8, absmean γ, int8 ±127) and bitnet.cpp I2_S facts, the
   repository's 2-bit code-table decision (kept for file compatibility;
   any 3-of-4 permutation is valid), and what is and is not reproduced
   from the 2B-4T recipe.
7. **Verified locally, all gates green:** `make test-run`
   → **174/0** (x86 baseline raised 142 → 174, ARM baseline 151 → 183
   in `tools/test_count_baseline.txt`, +32 arch-independent v3 checks);
   `tools/gguf_test.py` → 73/0; `gcc -fanalyzer` clean; cppcheck
   (src/ + include/) 0 defects; clang-tidy clean; flawfinder
   --minlevel=4 no new findings; `python3 -m py_compile` on both
   converters and the test gate.

## 2026-10-26 (round 3) — macOS release fix + PR-tier portability scan, size pin, full audit
## 2026-10-26 (round 3) — macOS release fix + PR-tier portability scan, size pin, full audit

1. **release.yml macOS build failure fixed (root cause: feature-test
   inversion).** release.yml's `build-macos` job
   (`clang -O3 -Wall -Wextra -Iinclude src/*.c`, macOS arm64, no -D
   flags) failed after the PR #55 merge: the in-file
   `_POSIX_C_SOURCE 200809L`/`_DEFAULT_SOURCE` defines *enable* the
   POSIX symbols on glibc but Darwin headers treat `_POSIX_C_SOURCE`
   as a **restriction** and hide the BSD extensions `O_NOFOLLOW` and
   `MADV_SEQUENTIAL` (neither is in any POSIX version). Fix: the
   define block in `src/hydra_engine.c` + `src/main.c` is now
   `#if !defined(__APPLE__) && !defined(__MACH__)`. All compile modes
   re-verified (strict C99 standalone, exact CI 1a with -D flags +
   -Werror, C++ parse, C suite 142/0 + 86/0 + 7/0, zig aarch64 full
   compile).
2. **Portability now fails on PRs, not in the release.** New
   `macos-strict-scan` job in ci.yml's PR tier: the exact release-macos
   command with `-fsyntax-only` (no binary, no artifact) +
   `tools/apple_portability_check.sh` (new, R25 self-testing:
   preprocessor-output asserts that Apple configuration self-defines
   no feature macro, plain configuration self-defines both, plus a
   broken-pattern fixture). Local proof of the macro logic; the
   Darwin-header proof is the job itself (measured: forcing
   `-D__APPLE__` on glibc is not a valid Darwin simulation).
3. **`sizeof(HydraEngine)` documented correctly and pinned.** Measured
   664 B on x86-64; the README's 656 B was the pre-`rng_state` size
   (stale). Fixed in README (2 places) and `docs/ARCHITECTURE.md` now
   carries the full per-field derivation; `include/hydra_model.h` pins
   664 at compile time on every platform
   (`hydra_engine_size_check`, C99-portable) — layout drift is now a
   build error.
4. **Evidence-based audit (`.agents/docs/TECHNICAL-AUDIT.md`)**: verified the
   zero-heap engine claim (true for `src/hydra_engine.c`;
   `src/main.c:479` mallocs a ≤512 B CLI buffer), the SIMD parity
   tests (all present, suite green), and the hardening in `server.js`;
   documented the mathematical limits of the v1 format (after
   load-time aggregation the whole model is 128 integers ≈ 208 bytes
   of capacity; decoding reads only dimension 0; the GGUF converter
   keeps ≤0.0075 % of a 7B model by construction), the M4 primitives
   (unit-tested, not wired into the forward pass), and the honest
   spec-only status of `.hydra2`; ranked the remaining hazards (mmap
   TOCTOU SIGBUS by design, accepted) and a prioritized P0-P3
   evolution plan.
5. **Local linter stack re-run (pip wheels, incl. flawfinder 2.0.20
   added to the recorded venv path):** gcc strict `-Werror -fanalyzer`
   rc=0; cppcheck `--enable=all src/ include/` 0 error / 0 warning /
   10 style (all exported APIs used from `tests/`, outside scan scope —
   0 real defects); clang-tidy 155 findings, 0 real analyzer bugs, 0
   concurrency findings; flawfinder `--minlevel=4` no hits;
   `make test-run` rc=0; gguf_test 59/0; server_test 37/37;
   test_count 142 vs baseline.

## 2026-10-26 — Local lint stack (no CI deferral), PR/main CI split, code-scanning alert handling, .agents/Environment.md

Owner round: "stop deferring C++ checks to CI" (with a tested pip path),
"PR runs = linters only, no full builds/publishing", "CI reacts to code
scanning results — not to issues, nothing hardcoded", plus a shared
environment record for all AI agents.

1. **Local C/C++ verification now runs locally, with proof.** The pip
   wheel path works in the Arena sandbox (no apt needed):
   `/tmp/lintenv` venv with Cppcheck 2.22.0, clang-tidy (LLVM 23.1.4),
   ShellCheck 0.11.0, yamllint 1.38.0, cpplint 2.0.2. The strict pass
   (`gcc -std=c99 -Wall -Wextra -Wpedantic -fanalyzer`, `g++
   -fsyntax-only -fanalyzer` C++ parse, cppcheck `--enable=all`,
   clang-tidy bugprone/clang-analyzer/cert) caught and fixed **six real
   pre-merge defects**: a `goto` crossing an initializer in
   `hydra_engine_load` (ill-formed under a C++ parse), missing
   feature-test macros under strict `-std=c99` in both `src/` files
   (`O_CLOEXEC`/`O_NOFOLLOW`/`clock_gettime` now self-contained), an
   uncast `malloc` + a `= {0}` aggregate in `main.c` (C++ parse),
   missing final newlines in `src/hydra_engine.c`, `src/main.c` and
   `.github/workflows/lint.yml`, an unguarded `cd` in
   `tools/workflow_yaml_check.sh`, and dead variables in
   `tools/string_format_negative_test.sh`. All suites re-run green after
   the fixes (142/0, 86/0, 7/0, count vs baseline). Remaining linter
   output is classified in .agents/Environment.md (intentional bit ops, C++-style
   noise on a C99 codebase, by-design exported M4 primitives).
2. **CI tier split: PR = lint-only, main = full build + publish.**
   `ci.yml` heavy jobs (`neon-test` QEMU, `build-test` x86/macos,
   `gpu-bench`, `android-apk`) now run on main pushes only; pull requests
   and feature-branch pushes get the fast lint tier (C gates 1a–1h,
   `jni-signatures`, `llama-pin`) plus the full `lint.yml` — whose
   `sanitizers` job already runs the entire unit suite under ASan+UBSan,
   so PR feedback keeps runtime test coverage without any artifact
   build. `release.yml` was already main/tag/dispatch-only and is
   untouched.
3. **Code scanning alerts replace the issue-mapping workflow (nothing
   hardcoded).** `tools/issue_verify.py`,
   `.github/issue-verifications.yml` and
   `.github/workflows/issue-verify.yml` are removed. New
   `.github/workflows/code-scanning.yml` triggers on the
   `code_scanning_alert` event; every reported fact (rule, file, line,
   message, commit, alert URL) is read from the event payload at run
   time — no rule lists, no issue numbers, no expected results. Delivery:
   PR comment when the alert's commit belongs to an open same-repo PR
   (de-duplicated by marker), job summary otherwise. Closure is native:
   GitHub resolves the alert as *fixed* when the next analysis no longer
   reports it — the fix commit closes it autonomously.
4. **.agents/Environment.md (new, in-repo): the shared environment record for all
   AI agents.** Describes each sandbox (what it is, egress allowlist,
   present/blocked tools with date + exact error + workaround) and the
   *tested* local check paths per environment. Contract (AGENTS.md §6):
   each agent updates **its own section only**, reads other agents'
   tested paths before claiming a tool missing, records every block with
   evidence, and never silently defers a locally-runnable check to CI.
   The first section (Arena AI, Debian 12) records the pip-wheel linter
   path above and the measured blocks (apt/Docker/QEMU/Android SDK,
   code-scanning API 403) with their workarounds.
**6. First-run CI red found + fixed with a regression test.** The first
   PR run failed in ci.yml gate 1a: that step compiles **five**
   translation units (src + three tests) **with**
   `-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE`, while the local pass
   had compiled two files without the flags — the new in-file
   feature-test defines then hit `-Werror` as "redefined". Fix:
   `#ifndef`-guarded defines in both `src/` files (standalone *and*
   command-line-flagged compiles verified rc=0); permanent guard:
   `tools/ci_syntax_mirror_test.sh` runs the **exact** ci.yml syntax
   commands (parsed from the workflow at run time, vacuous-parse guard,
   negative control proven against the pre-fix files) — now also a
   ci.yml lint step so future drift fails in the same run. AGENTS.md
   gate list updated.

5. **AGENTS.md updated accordingly:** §3 adds the mandatory local
   C/C++ static-analysis gate (1b) and the "read .agents/Environment.md first"
   rule; §6 carries the .agents/Environment.md contract; §8 now documents the
   code-scanning alert channel (and explicitly that CI reacts to
   scanning results, not to issues, and that results are never
   hardcoded); §5 table lists .agents/Environment.md and the new workflow.

## 2026-10-26 — Compose crash fix + regression guard, Android CI caches, issue auto-close, research memory in repo

Owner round after the M1 close-out (crash report, "füge die Memory zum
Repo hinzu", missing tools, README audit, CI speed-up, auto-close).

1. **Chat-tab crash fixed (`HydraApp.kt`) — regression from PR #50.**
   Every chat-tab composition on Android (reported 2026-10-26, Android 15
   / arm64 / API 35) threw
   `IllegalStateException: … measured with an infinite max height and
   Compose doesn't support this` (foundation 1.7.3, Compose BOM
   2024.09.03). Root cause, proven by mapping every crash frame 1:1 onto
   the foundation v1.7.x source (identical across v1.7.0–v1.7.3): the
   ChatTab header — itself `Modifier.verticalScroll` — sits inside the
   list body's `Column { verticalScroll { LazyColumn { … } } }` across a
   composable boundary, so the inner scrollable measures with an
   unbounded max height and the framework's
   `CheckScrollableContainerConstraints` throws. Fix: the header is
   de-scrolled; `AdaptiveHeaderSplit` keeps the header in a bounded
   `BoxWithConstraints` height cap and applies `verticalScroll` only when
   the cap binds — exactly one vertical scrollable per column, an
   invariant now documented in the KDoc.
2. **Regression guard for the crash class (new, permanent).**
   `tools/compose_scroll_guard.py` detects (a) nested same-axis
   scrollables within one strict source region and (b) the boundary case:
   a scrollable at a lambda call site whose composable body (same file)
   contains another same-axis scrollable or lazy list. `tests/fixtures/
   compose/` carries the negative/positive controls (2 bad files with the
   exact expected finding counts incl. the real crash shape; 2 good files
   with legal shapes, zero findings). `tools/compose_scroll_check.sh` is
   the self-testing gate, wired into CI as lint job gate **1g**.
   Verified before shipping: the pre-fix file (commit `d0c7679`) is
   detected with exactly the real pair; all 13 shipped Compose sources are
   clean; fixture counts match (R25).
3. **Android CI jobs cached (measured first, changed second).** Measured
   on the 2026-10-26 runs: the QEMU leg is 23 s — the actual CI cost was
   the two uncached Android jobs (APK 7 m 23 s, lint 4 m 38 s), each
   re-downloading the SDK and Gradle. `.github/actions/setup-android`
   now roots everything under `$HOME` (cacheable), skips all downloads
   when every pinned package is already present, and takes a `gradle`
   input; both Android jobs restore/save `actions/cache` — one shared
   SDK key (identical pins) plus a dependency-cache key hashed over the
   Android build files. A cache miss reproduces today's path exactly
   (zero risk); a red build never overwrites a good dependency cache.
   Expected: every Android job after the first warm run skips the
   downloads; QEMU left as is (not the bottleneck).
4. **Issue auto-close with proof (new, stdlib only).** Research on 2026
   patterns (gh-CLI close with `issues: write`; the amd/gaia
   "close only when the check ran and passed" reference) led to:
   `tools/issue_verify.py` + `.github/issue-verifications.yml` (issue →
   gates mapping) + `.github/workflows/issue-verify.yml` (push → main and
   manual dispatch). The workflow runs exactly the mapped gates and
   closes the issue via `gh issue close --comment` **only when every
   gate passes**, with an audit comment (summary, `main@<sha>`, run URL,
   gate list, reopen note); `--dry-run` exists and was exercised
   end-to-end. Convention (AGENTS.md §8): ordinary issues close
   natively with `Closes #N` in the PR body — only issues a PR body
   cannot close get a mapping entry. First entry: issue #52 (gates: the
   new compose guard + `make clean && make -j2 test-run`).
5. **Research memory now lives in the repository.**
   `.agents/docs/RESEARCH-LOG.md` is the permanent in-repo record of
   investigations (crash line map, CI cache architecture, auto-close
   research, code-scanning access findings, sandbox tool inventory with
   the exact install channels that work/fail here). The `.gitignore`
   rule that kept it local was removed; AGENTS.md §5 states the path
   must be kept current.
6. **AGENTS.md extended.** §3 gains the compose guard; new §6
   (missing tools → install them before claiming "blocked", with the
   measured sandbox channels — incl. that Docker is not installable in
   the Arena sandbox because the APT sources are off the egress
   allowlist, and no gate needs a container runtime), §7 (crash
   protocol: no crash is done without a regression test that fails on
   the pre-fix source, wired into CI), §8 (issue closing conventions).
7. **README roadmap audited against the code.** All 7 roadmap claims
   verified true (unwired forward pass, min-P sampling, stdlib-only
   safetensors import, AVX2 + "AVX-512 not implemented", v2 4-per-byte
   packing with the three decode paths and parity gates, `.hydra2`
   unimplemented, sink KV cache); one precision note added: the NEON v2
   unpack kernel is AArch64-only (32-bit ARM uses the bit-identical
   scalar decode).
8. **Workflow-YAML duplicate-key gate (CI gate 1h).** The first push of
   this PR turned `lint.yml` into a zero-job red run: its dependency-
   cache save carried a duplicate `key:` line. PyYAML (and most YAML
   tooling) silently accepts duplicate mapping keys — last one wins —
   so a plain parse check reports ok, but GitHub's workflow compiler
   rejects them at load time: the run is created with zero jobs and
   fails in the same second, with no job log to read. `tools/
   workflow_yaml_check.sh` parses every `.github/**/*.yml` with a
   duplicate-key-rejecting loader (PyYAML on the runners, a stdlib
   indent-scan fallback) and proves the detector with the
   negative-control fixture `tools/fixtures/workflow_yaml_bad.yml`
   (R25). Both parser paths verified green.

## 2026-10-26 — NEON v2 test heap overflow fix + 32-bit ARM gate (third CI red run)

The new in-step `::error::` diagnostics did their job and named both
remaining failures precisely.

1. **Runtime heap overflow in `test_neon_v2_load_matches_scalar`
   (macOS arm64 + qemu-aarch64, same bug).** The ASan rebuild added to
   the macOS unit-test step reported
   `heap-buffer-overflow: WRITE of size 1 at 0 bytes after 16-byte
   region, test_engine.c (v2 load test)`: the test allocated
   `malloc(dim / 4)` = 16 bytes for each of `pa`/`pb` but wrote
   `layers * dim / 4` = 48 bytes (3 layers × 16). The plain binary
   survived the overflow until the heap metadata was hit (macOS:
   "Trace/BPT trap", exit 133; qemu/glibc: `malloc(): corrupted top
   size` + SIGABRT in the neon-test job). Fixed: allocate
   `layers * (dim / 4)`. All other `malloc`s in the test file were
   audited (v1 models correctly use `dim * layers`).
2. **Android `armeabi-v7a` (32-bit ARM) cannot compile the v2
   kernel.** The APK build compiles `src/hydra_neon.h` for the 32-bit
   target and failed with
   `call to undeclared function 'vqtbl1q_u8'`: the 128-bit
   table-lookup intrinsic exists only in the AArch64 headers (32-bit
   ARM provides the differently shaped `vtbl1_u8` instead). The v2
   unpack kernel is therefore compiled for AArch64 only, via a new
   `HYDRA_NEON_V2_UNPACK` macro in `src/hydra_neon.h` (defined when
   `__ARM_NEON` and `__aarch64__`/`__arm64__`/`_M_ARM64`). On 32-bit
   ARM the engine takes the existing SCALAR v2 decode at load time —
   bit-identical result; the difference is one model load, not every
   token. The existing step kernel (hot path, dual-ABI intrinsics) is
   unchanged and still runs on 32-bit ARM.
   - `src/hydra_neon.h`: `HYDRA_NEON_V2_UNPACK` macro; the v2 kernel
     + `hydra_neon_u8x16_as_s8` gated with it.
   - `src/hydra_engine.c`: the `use_neon` wiring uses
     `HYDRA_NEON_V2_UNPACK` instead of `__ARM_NEON` (32-bit ARM falls
     to the scalar decode).
   - `tests/test_engine.c`: the two v2 tests and the shared
     `ref_unpack_v2_plane` helper are gated with
     `HYDRA_NEON_V2_UNPACK`/`__AVX2__` (the helper must not be
     compiled where no caller exists — the same `-Wunused-function`
     trap the CI clang gate caught on x86). Test counts are
     UNCHANGED: the qemu-aarch64 (151) and macOS (151) builds define
     the macro, the x86 build (142) never did.
- **`tests/test_engine.c`**: `pa`/`pb` allocated `layers * (dim / 4)`
  (was `dim / 4`).

Verified locally: x86 gate green (142/0, jni 86/0, large 7/0, zero
warnings), CI clang-syntax config clean (both TUs, no-AVX2 x86,
`-Werror`), baseline `[471, 386, 385, 386, 385, 386, 385, 386]` @ 42
unchanged, clean zig-clang aarch64 build of both TUs with all 11 v2
assertion strings present in the binary (0 in the x86 binary).

## 2026-10-26 — NEON kernel portability fix (real ARM compilers reject the intrinsics as written) + CI failure diagnostics

CI run on `5c1f145`: the `lint` job failed because `ref_unpack_v2_plane`
became an unconditional static in `tests/test_engine.c` but is only
called from the `__ARM_NEON`/`__AVX2__` test blocks — the x86 no-AVX2
clang syntax check sees it as unused (`-Werror -Wunused-function`; gcc
is silent under `-fsyntax-only` because it only emits that warning
during codegen). Fixed by guarding the helper with
`#if defined(__ARM_NEON) || defined(__AVX2__)` (`e1c6c3b`).

The following run then exposed the real defect: `neon-test`
(aarch64-linux-gnu-gcc-12) and the macos-latest `build-test` leg
(Apple clang, arm64) both fail to COMPILE the new kernel, while the
x86 leg never compiles NEON at all. The kernel's intrinsics were
written against the ARM instruction set but not against what the
two CI toolchains' `arm_neon.h` headers actually accept:

1. **Negative `vshlq_n_u8` immediates are not portable.**
   `vshlq_n_u8(bytes, -2/-4/-6)` is rejected by the AArch64
   `arm_neon.h` implementations (the static-immediate macro/builtin
   only encodes a positive range — `argument value -2 is outside the
   valid range [0, 7]`). Replaced by `vshrq_n_u8(bytes, 2/4/6)` —
   the logical right shift, in the legal `LSR` immediate range of
   every AArch64 implementation. Semantics are unchanged: for the
   LUT index only `shifted & 0x03` matters, and the low 2 bits after
   a shift are identical for the arithmetic and the logical variant
   (the sign-fill bits always land above the mask) — confirmed by
   the 200,000-plane shadow proof against the scalar decoder, which
   used logical shifts.
2. **The unsuffixed `vreinterpretq_s8` does not exist in the AArch64
   headers of either CI toolchain.** The first fix attempt selected
   the spelling by `__clang__` (unsuffixed for GCC) and the next CI
   run proved that wrong with the actual compiler error:
   aarch64-linux-gnu-gcc-12 reports
   "implicit declaration of function 'vreinterpretq_s8'; did you
   mean 'vreinterpretq_u8_s8'?" — GCC's AArch64 `arm_neon.h` also
   uses only the source-suffixed names (the unsuffixed spelling is a
   32-bit-ARM header convention). The kernel now calls
   `vreinterpretq_s8_u8` unconditionally via the
   `hydra_neon_u8x16_as_s8()` macro — the one spelling both AArch64
   toolchains declare.

The defects were found by reproducing the aarch64 compile locally
with a zig-bundled clang toolchain (`pip install ziglang`; the
sandbox has no aarch64 gcc) — it emits exactly the errors the CI
compilers report. Verification after the fix: clean aarch64
cross-compile of both TUs (`-O2 -Wall -Wextra`), full x86 gate
re-run green (142/0, jni 86/0, large 7/0), baseline
`[471, 386, 385, 386, 385, 386, 385, 386]` @ 42 unchanged, test
counts untouched (x86 142 / arm 151).

- **`src/hydra_neon.h`**: `vshlq_n_u8(bytes, -N)` →
  `vshrq_n_u8(bytes, N)` (2/4/6); `vreinterpretq_s8(v)` →
  `hydra_neon_u8x16_as_s8(v)` = `vreinterpretq_s8_u8(v)` (the only
  spelling both AArch64 toolchains declare).
- **`tests/test_engine.c`**: `ref_unpack_v2_plane` now compiled only
  where it is used (`__ARM_NEON || __AVX2__`) — CI syntax gate green
  on the x86 no-AVX2 clang check again.
- **`.github/workflows/ci.yml`**: the `neon-test` build+run step and
  the `build-test` build/run steps now capture the real compiler /
  test output and, on failure, mirror it into check-run `::error::`
  annotations before exiting — the log/blob hosts are not reachable
  from every network, and the check-run annotation API is the
  reliable read path for these jobs (proven: the real GCC error
  above arrived through exactly this channel). On top of that, the
  macos-arm64 unit-test step now rebuilds the same sources with
  `-fsanitize=address,undefined` on a runtime failure and mirrors the
  ASan report (faulting instruction + stack) into annotations — the
  plain binary's buffered stdout is lost in a segfault, so the
  report is the only channel that shows WHERE the fault happened.

## 2026-10-26 — M1 completion: ARM NEON in-register v2 plane unpack (2-bit decode at load)

Closes the last gap of milestone M1 ("True In-RAM 2-bit packed
inference, 4× weight memory cut"): the ARM64 engine decoded the
version-2 weight planes with the pure scalar loop while x86-64 already
had an in-register AVX2 LUT path. The in-RAM objective itself was
already met by the mmap + load-time aggregation design (resident weight
memory is only the two 64-dim int32 A/B aggregates; the mapped pages are
released after aggregation, RSS delta measured at 0 KiB) — this entry
adds the NEON kernel and its parity proof.

- **`src/hydra_neon.h`**: new `hydra_neon_unpack_v2_plane()` (static
  inline, compiled only when `__ARM_NEON` is defined). Per 16-byte
  chunk (64 dimensions) the four 2-bit weights are normalized into the
  low 2 bits (logical right shifts via negative `vshlq_n_u8`
  immediates — a pure AND mask like `0x3C/0xF0/0xC0` cannot work,
  because `vqtbl1q_u8` indexes with each lane's LOW 4 bits only),
  masked with `0x03`, and decoded through the same 16-entry
  `T[i] = ternary(i & 3)` table the scalar and AVX2 decoders use (16
  bytes, `aligned(16)` for the `vld1q_u8` load, −1 as 0xFF). The
  decoded planes are widened `s8 → s16 → s32` (sign-extension carries
  the −1) and a 4-way interleave in two `vzipq_s32` rounds (pairs two
  apart: `q02 = zip(r0,r2)`, `q13 = zip(r1,r3)`, then re-zip groups
  instead of alternating) yields the exact dimension order
  `(R0,R1,R2,R3)` per byte; the result accumulates into `agg` with
  `vaddq_s32`. Contract: `dim % 64 == 0`, one plane call per 16-byte
  window. Bit-identical to `hydra_build_aggregation_v1_1` by
  construction: one integer from the same table per dimension into the
  same int32 accumulator.
- **`src/hydra_engine.c`**: the v2 branch of
  `hydra_build_aggregation()` now mirrors the AVX2 block: `use_neon =
  dim % 64 == 0` (no runtime probe needed — NEON is base ISA on
  AArch64, and a build without NEON never defines `__ARM_NEON`, so the
  kernel is simply not compiled in). Per layer, 64-dim chunks are
  unpacked by the kernel for A and B; any other dimension falls through
  to the scalar decoder unchanged.
- **`tests/test_engine.c`**: +11 ARM-only checks (compiled under
  `#ifdef __ARM_NEON`, i.e. the CI `qemu-aarch64-static` leg):
  `test_neon_v2_unpack` — random 3-layer dim-64 planes vs the
  independent scalar reference (the kernel's multi-layer ADD path),
  the full 4-code table sweep with per-code hand-pinned sums (00→0,
  01→+1, 10→−1, 11→0; catches a sign inversion or nibble-order swap),
  and a dim-128 plane (two 64-dim chunks) exercising the kernel's
  `d += 64` loop; `test_neon_v2_load_matches_scalar` — a seeded v2
  model with a proof that all four codes occur, loaded through the
  engine (kernel live on this build) with `agg_a`/`agg_b` required to
  be bit-identical to an in-test scalar decode of the same raw file
  bytes, plus a 32-token stream against the documented step formula
  driven by the independently decoded A/B (the reference never sees
  the kernel). The arch-neutral helpers `wr_header_version` and
  `ref_unpack_v2_plane` moved above the NEON block so both the NEON
  and the AVX2 v2 suites share one independent reference (no
  duplication, no use-before-definition).
- **`tools/test_count_baseline.txt`**: arm 140 → 151 (the 11 checks
  compile only into NEON builds); x86 unchanged at 142.

Measured (x86-64 sandbox — NEON verifies on the CI qemu-aarch64-static
leg; kernel math additionally proven on x86 by an element-level shadow
model of the lane movement: 200,000 random dim-64/128 planes
bit-identical to the scalar reference): `make hydra-test` zero
warnings, `./hydra-test` 142/0 (baseline run
`[471, 386, 385, 386, 385, 386, 385, 386]` @ 42/8 unaffected — the
v1 model does not touch the v2 branch at all).

## 2026-10-25 — Android keyboard + adaptive chat layout; full .md audit; local scan helper

**Android UI (fix for "the UI breaks while typing")**

- `android/app/src/main/res/values/themes.xml` +
  `values-night/themes.xml` (new): `Theme.HydraStone`, a `NoActionBar`
  platform theme (light + night, `windowLightStatusBar` matching the
  Compose scheme). The old
  `Theme.Material.Light.DarkActionBar` drew a second "Hydra-Stone" bar
  above the Compose `TopAppBar` — the title was doubled and the vertical
  space was eaten exactly where the keyboard needs it. The manifest now
  points at the app theme.
- `MainActivity.onCreate()`: on API 30+ the window goes edge-to-edge
  (`enableEdgeToEdge()`) with `SOFT_INPUT_ADJUST_NOTHING` — the soft
  keyboard then arrives as a real `WindowInset` and the composer's
  `Modifier.imePadding()` lifts the input row by exactly the keyboard's
  height, whatever it is (split keyboard, emoji row, swipe-up). Below 30
  the platform delivers no IME insets, so the classic `adjustResize`
  stays on: the window shrinks and the layout below adapts.
- `ui/HydraApp.kt`: new `AdaptiveHeaderSplit`, a custom `Layout` that
  measures the header block (banner, model row, engine status/selector,
  auto-run) at its natural height up to 45% of the space and hands the
  rest to the conversation. A plain `weight()` cannot express this — a
  weighted child never returns its unused share, so the composer would
  float above the bottom edge. The header is internally scrollable; the
  conversation `LazyColumn` drops the old fixed `heightIn(min = 120.dp)`
  floor, which overflowed the window exactly when the window was
  smallest (keyboard open on a short screen), and gets a relative share
  instead. The input row is a plain sibling after the weighted split and
  can no longer be pushed off screen.
- `tools/android_autostart_check.sh`: the "guaranteed minimum height"
  check now asserts the new mechanism — the header block is capped at a
  fraction of the space (grep on the cap EXPRESSION
  `maxHeight * headerFraction`, not the parameter alone, so a cap in
  name only fails) plus the remainder hand-over to the content; the
  nested-scroll check now scopes on `MessageList`.
  `tools/android_autostart_negative_test.sh` (the CI-side proof that
  every gate check can fail) is updated to match: the old
  min-height scenario is replaced by two scenarios, one per new check,
  each removing the real wiring — all 15 scenarios now turn their
  check red (15 proved, 0 could not fail).

**Docs**

- `docs/ARCHITECTURE.md`: the SIMD table's NEON row cited a function
  that does not exist, `hydra_neon_accumulate_chunk()` — the real
  kernel is `hydra_neon_accumulate_agg()` (16 values per fixed-width
  chunk, 4 int32 lanes per vector op). Name and wording corrected.
- `CHANGELOG.md`: the M3 entry sat above M6, violating the
  "newest entries first" order the file's own header promises.
  Reordered.
- All 21 tracked `.md` files audited: no non-English text, no
  hard-coded test counts in the live documents, every relative link
  resolves, every `hydra_*` symbol named in a doc exists in the code,
  every README roadmap `[x]` box cites verifiable code, and the engine
  still reproduces the quoted deterministic token baseline.

**Local tooling (the script itself is gitignored; the ignore entry is
committed)**

- `tools/local_scan.py` — a 10-check local scanner (non-English text,
  hard-coded counts, links, stale phrasings, doc-vs-code symbols,
  roadmap boxes, quoted baseline, parsed test count, changelog order,
  self-gitignore). The committed CI gate remains
  `tools/docs_claim_check.sh`; this is the "scan everything" aid.

### CI follow-up (same day, after the first CI run)

The first CI run on the PR found three defects the sandbox cannot
reproduce (no Android SDK, no aarch64 toolchain, no CodeQL):

- **Kotlin compile errors in `AdaptiveHeaderSplit`**: the custom
  `Layout` implementation imported `Constraints` from the wrong package
  (`androidx.compose.ui`, not `foundation.layout`) and the `Layout`
  import did not resolve in this Compose set at all. Replaced the whole
  mechanism with a `BoxWithConstraints` implementation — the cap is
  read from the incoming constraints in the same measurement pass (no
  size-callback state, no collapsed first frame), and every API used
  already resolves in this file (`BoxWithConstraints` sits next to
  `Box` in the same `foundation.layout` package; the `Dp` arithmetic
  goes through `maxHeight.value` + the already-imported `.dp`
  extension, so no new operator import is needed).
- **aarch64 static link failure** (`undefined reference to
  sqrtf/pow/sincos/expf/exp`): the "Cross-arch determinism check" step
  in `.github/workflows/ci.yml` linked `hydra-run-arm` without `-lm`;
  the M4 float primitives made libm mandatory. `-lm` added (the
  sibling `hydra-test-arm` line in the same job already had it).
- **CodeQL (built-in) alert in `tests/test_engine.c`**:
  `make_v1_model` used `fopen(path, "wb")`, which creates a missing
  file with the internal 0666 mode (world-writable under a permissive
  umask). Now `open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644)` + `fdopen`,
  with the reasoning in a comment.
- The autostart gate + negative control were updated in the same round:
  the cap check greps the cap expression
  (`maxHeight.value * headerFraction`), the remainder check greps the
  body column's `weight(1f)`, and the two negative scenarios break
  exactly those wirings. Local proof: gate 32/0, negative control 15
  proved / 0 could not fail, `hydra-test` 142/0.

### CI follow-up, round 2-4 (same day): API-readable diagnostics + the px/Dp root cause

The second CI run showed that the compile errors had moved, but the
sandbox cannot read Actions job logs (the log-download and artifact
hosts are egress-blocked; the check-run output API does not index step
summaries; PR-comment posting is refused by the repository token
policy). The two Android jobs now therefore publish their failure from
the build step itself as a **check-run annotation** — one `::error::`
line with the Kotlin `e:` compiler lines plus a short gradle log tail
(lint job: the same, plus per-finding annotations when the lint report
exists). Annotations are served by the API and need no permissions.
The lint job also gained `issues:write` (the PR-comment mirror stays
for when the token policy allows it).

The annotation from run 4 of the round delivered the error text:
`HydraApp.kt:340:36 Unresolved reference 'value'`. Root cause:
`BoxWithConstraintsScope.constraints` is the **pixel-based**
`androidx.compose.ui.unit.Constraints`, whose `maxHeight` is a plain
`Float` — it has no `.value`. (Verified against the foundation
1.7.3-era sources, which also show the scope exposing the Dp-converted
value as its own `maxHeight` property.) The cap now uses that scope
property: `(maxHeight.value * headerFraction).dp` — no new imports.
`SOFT_INPUT_ADJUST_NOTHING` (deprecated since API 30, and not what
edge-to-edge wants — a non-resizing window on API 30+ delivers the IME
as an inset under the default soft-input mode) was dropped, which
also cleared the "0 findings" lint gate. The negative-control
scenario for the cap is re-anchored to the new expression.

Result: **all CI green** (runs 40237612415 "CI" + 40237612492
"Lint & Security" on commit `5445742`): Android APK build, Android
lint, neon-test (incl. cross-arch determinism), CodeQL built-in /
c-cpp / js, sanitizers, docs claim lint, and every other leg pass.

## 2026-10-25 — M6: docs + baseline (roadmap checkboxes only for verified work)

- **`README.md`**: the Roadmap now reflects reality, with **exact
  function references** on every checked box and no fake checkboxes:
  - `[x]` Min-P sampling & repetition penalty → `hydra_sample_token()`
    (`include/hydra_model.h`, `src/hydra_engine.c`), CLI
    `--temp/--min-p/--rep-penalty` (`src/main.c`), tested in
    `tests/test_engine.c`.
  - `[x]` safetensors import → `tools/safetensors_to_hydra.py`,
    round-trip-gated in `tools/gguf_test.py` (`run_safetensors`).
  - `[x]` AVX2 kernel for x86 → `src/hydra_simd.h`
    (`hydra_avx2_accumulate_agg`, `hydra_avx2_unpack_v2_plane`), runtime
    dispatch + scalar fallback, parity-tested; **AVX-512 explicitly not
    implemented.**
  - `[x]` Streaming ring-buffer KV with attention sinks →
    `hydra_kv_cache_t` + init/append/size/get/sink,
    eviction-with-sinks-preserved tested.
  - Still `[ ]`: the full transformer forward pass (the M4 primitives exist
    and are tested but are not yet wired into a forward pass) and the
    `.hydra2` sparse-MoE container (nothing implemented). The NEON/AVX2
    honesty note is updated (x86 is no longer "purely scalar").
- **`docs/ARCHITECTURE.md`**: Components table gains `src/hydra_simd.h`,
  `tools/gguf_to_hydra.py`, `tools/safetensors_to_hydra.py` and the sampler
  + primitives in the API/engine rows; the SIMD Strategy table now lists the
  active AVX2 path (8 dims/256-bit iteration, runtime dispatch, plain-build
  scalar) with the int32-exactness bound; a new "Transformer Primitives and
  Streaming KV Cache (M4)" section documents the four guarded building
  blocks and the int8 bridge.
- **`docs/GGUF-IMPORT.md`**: the short version names the safetensors
  command; a new §10 documents the safetensors container parsing/
  both-ends offset validation, the F32/F16/BF16 decode set, the absmean
  ternarisation, the version-2 output (first shipped writer for that
  packing) and the three-way round-trip verification.
- **`AGENTS.md`**: the stale hard-coded "38 checks" for `gguf_test.py` is
  replaced with the repo's own convention — "expect `N checks, 0 failed`
  (N parsed, never assumed)".
- **`tools/test_count_baseline.txt`**: unchanged at x86 142 / arm 140 —
  M5 added no C tests (its checks live in `gguf_test.py`, which is an
  exit-code gate, not a count-gated binary), so the measured C count is
  unchanged and the baseline already matches.

## 2026-10-25 — M5: safetensors import -> version-2 .hydra (4 ternary weights/byte)

- **`tools/safetensors_to_hydra.py`** (new, Python standard library only):
  reads a safetensors container (8-byte LE header length + JSON header +
  data region), validates every tensor's `data_offsets` at BOTH ends
  (`start >= 0`, `end > start`, `8+N+end <= file_size`, and the span equals
  `n_elements * bytes_per_element`) and refuses malformed input by name -
  a header length past the end, an unparseable header, an unsupported dtype
  (only F32/F16/BF16), a 3-D tensor, out-of-order offsets or a range that
  runs past the file end. Decodes F32 (struct), F16 (IEEE binary16 via
  `<e>`) and BF16 (top 16 bits of an F32).
  - Ternarises each plane with the b1.58 absmean rule: `gamma = mean|W|`,
    `W_q = clip(round(W/(gamma+1e-8)))` with half-away-from-zero rounding
    (the +1e-8 keeps an all-zero tensor finite). The ternary column sums
    are two counts per column (the threshold form of the rounding), so the
    ternary matrix is never materialised.
  - Folds the column sums to the 64-wide state and writes a **version-2**
    `.hydra` (4 ternary weights per byte, two `dim/4`-byte planes per
    layer) whose aggregation A/B reproduces the folded sums value for
    value - the same exactness guarantee as the v1 GGUF converter.
- **`tools/gguf_test.py`**: a `run_safetensors` round-trip gate (called
  from `run()`), proven three independent ways:
  1. **Packing** - `read_hydra_v2` re-derives A/B from the packed bytes
     with no shared code and compares to the reported A/B (catches a
     transposed bit order or a swapped plane).
  2. **Quantisation** - an oracle written from the spec's literal rule
     (element-wise half-away round-clip, its own decode and fold) must
     equal the reported A/B (catches a wrong gamma/rounding/threshold).
  3. **The real engine** - `./hydra-run` on the v2 file is compared to a
     reference step, plus F32/F16/BF16-of-the-same-weights-convert-
     identically and six negative controls.

Measured: `gguf_test.py` 40 -> **59 checks, 0 failed** (the 19 new
safetensors checks; the C count is unchanged, so `test_count_baseline.txt`
needs no edit); the baseline `[471,386,385,386,385,386,385,386]` @ 42/8 is
untouched (the starter model is v1 and the v1 path is unchanged).

## 2026-10-25 — M4: Transformer primitives (RMSNorm/RoPE/SwiGLU) + streaming KV cache

- **`include/hydra_model.h` / `src/hydra_engine.c`** (new API, zero-heap,
  R14-guarded — every function validates args AND checks all input
  finiteness before any output store, so a -1 return never leaves a
  partially written result):
  - `hydra_rmsnorm(x, w, dim, eps, out)` — `y = x/√(mean(x²)+ε)·w`,
    in-place safe, dim 1..4096.
  - `hydra_rope_inplace(x, dim, pos, theta_base)` — standard rotary
    embedding on pairs (x[2i], x[2i+1]) with `freq_i = θ^(-2i/dim)`.
  - `hydra_swiglu(g, u, dim, out)` — `SiLU(g)·u` with a branch-free-
    overflow-safe sigmoid (`x≥0: 1/(1+e^-x)`, else `e^x/(1+e^x)` —
    no Inf on either tail).
  - `hydra_kv_cache_t` + `init/append/size/get/sink` — streaming
    attention cache: `num_sinks` permanent sink slots (streaming-LLM
    attention sinks, never evicted) + a circular window over
    `capacity - num_sinks` slots that overwrites its oldest entry.
    Caller-provided storage (zero heap), O(1) memory and O(1) per
    append/lookup. These are the float building blocks a real
    transformer head needs; the SwiGLU bridge test proves their
    outputs quantize into the engine's int8 state world with the
    engine's own clamp, idempotently.
- **`tests/test_engine.c`**: 21 new checks (all architectures):
  RMSNorm hand-computed value + per-dim weights + scale invariance +
  NaN/arg guards (negative controls, unmodified output verified);
  RoPE identity at pos=0 (bit-exact) + hand-computed rotation + norm
  preservation (orthogonality) + guards; SwiGLU hand-computed
  silu(1)·2 + both tails (g=±100, no overflow) + 256-sample
  float→int8 engine-clamp bridge + guards; KV cache init/fill,
  resident order (sinks then window, oldest→newest), eviction of the
  oldest WINDOW token (sinks intact) after the cache is full, a
  4-append window slide, sink permanence after 10 appends, and the
  OOB-get / NaN-append-no-partial-write / sinks>capacity controls.
- **`tools/test_count_baseline.txt`**: x86 121 → 142, arm 119 → 140
  (all 21 checks compile on every architecture).

Measured: `./hydra-test` 142/0, zero warnings; baseline run
`[471, 386, 385, 386, 385, 386, 385, 386]` @ 42/8 unchanged;
`jni-batch-test` 86/0, `large-model-test` 7/0, `gguf_test.py` 40/0.

## 2026-10-25 — M3: AVX2 x86-64 kernels (step kernel + v2 unpack LUT), runtime dispatch

- **`src/hydra_simd.h`** (new): the two AVX2 kernels, compiled only
  when the build carries `-mavx2` (`__AVX2__`):
  - **Step kernel** `hydra_avx2_accumulate_agg` — 8 dims per 256-bit
    iteration (a 256-bit lane holds 8 int32). The int32 lanes are
    provably exact: |A|,|B| ≤ 4096 (layer cap), |t| ≤ 1023, |s| ≤ 127
    → |A·t + B·s| ≤ 4,710,400 < INT32_MAX, so `mullo_epi32` +
    `add_epi32` cannot wrap, and the `cvtepi32_epi64` widening is a
    re-interpretation. Bit-identical to the scalar expression by
    construction (same integer ops); the parity test is the proof.
  - **v2 plane unpack** `hydra_avx2_unpack_v2_plane` — the 2-bit →
    signed LUT decode (one `shuffle_epi8` per weight position over a
    16-entry table, leak-proof masked 16-bit shifts, and the
    mandatory `& 0x0F` index mask because VPSHUFB zeroes any lane
    whose index has bit 7 set). Honest scope note: the 4-weights/byte
    scatter into stride-4 dimension slots stays a small scalar loop —
    a 4×16 byte transpose is not cleanly expressible with plain AVX2
    shuffles and the operation runs once at load time, not per token.
  - `hydra_simd_has_avx2()` — runtime CPUID probe
    (`__builtin_cpu_supports`), constant 0 in non-AVX2 builds.
- **`src/hydra_engine.c`**: `hydra_engine_step()` dispatches to the
  AVX2 kernel (8-dim chunks + scalar tail) when the CPU supports AVX2,
  otherwise the scalar loop — a `-mavx2` build therefore still runs on
  non-AVX2 x86 CPUs; a plain build contains no AVX2 instruction at all
  (no SIGILL risk). The v2 load path uses the unpack kernel for
  dim=64 (the standard dimension), scalar otherwise. The ARM NEON path
  is untouched (mutually exclusive architectures).
- **`Makefile`**: adds `-mavx2` only when `uname -m` is x86_64 AND the
  compiler accepts the flag (probe compile); the probe artifact is
  removed in `clean`. Cross-compilation (the ARM CI leg) never matches.
- **`tests/test_engine.c`**: 8 new x86-only checks (compile-guarded
  `#ifdef __AVX2__`, runtime SKIP on non-AVX2 CPUs — the NEON test
  pattern): runtime probe; step kernel vs scalar reference over 11
  dims (sub-chunk, mid-chunk tails, full chunks) with LCG-random
  matrices (R22 platform-fixed seed) and the layer-cap extreme
  (A=4096, t=1023, B=−4096, s=127 → 3,673,024 hand-computed); v2
  unpack vs an independent scalar reference (3 random layers, dim 64)
  and the full 4-code table sweep.
- **`tools/test_count_baseline.txt`**: x86 113 → 121 (the 8 AVX2 checks
  compile only into `-mavx2` builds), arm unchanged at 119.

Measured (x86-64 sandbox with AVX2, so the SIMD path is live):
`./hydra-test` 121/0; baseline run
`[471, 386, 385, 386, 385, 386, 385, 386]` @ 42/8 UNCHANGED on the
AVX2 build (end-to-end parity); `jni-batch-test` 86/0,
`large-model-test` 7/0, `gguf_test.py` 40/0; zero warnings.

## 2026-10-25 — M2: Min-P sampling + repetition penalty (greedy guard keeps the baseline)

Adds controlled randomness to the deterministic decoder without touching
the greedy path:

- **`include/hydra_model.h`**: new `hydra_sampler_config_t`
  (`temperature`, `min_p`, `repetition_penalty`, `penalty_window`,
  `rng_seed`) and `hydra_sample_token()`. `HydraEngine` gains one
  `uint64_t rng_state` field (zero after load; never read by greedy
  decode), so a full run is reproducible from (model, start token,
  config).
- **`src/hydra_engine.c`**: `hydra_sample_token()` implements the
  documented two-mode contract:
  - **Greedy guard** — `temperature <= 0` OR `min_p >= 1` delegates to
    `hydra_engine_step()`: bit-identical output, which is what keeps
    the deterministic baseline sequence valid.
  - **Sampling** — the engine has no native per-vocabulary logits, so
    the candidate logit is the engine's own quantity:
    `L(c) = agg_a[0]*c + agg_b[0]*state[0]` (the acc[0] that would be
    computed if c were fed). Then: repetition penalty over the recent
    window (positive logits divided, negative multiplied, repeats
    compound) → numerically stable softmax (max subtracted before
    exp) → min-p filter (drop `P(c) < min_p * p_max`, renormalize) →
    seeded xorshift64* CDF walk → state advance through the real
    `hydra_engine_step(chosen)`, so the state machine is the SAME as
    greedy mode. Zero heap (one 8 KiB stack array for the vocab cap).
- **`src/main.c`**: new flags `--temp`, `--min-p`, `--rep-penalty`,
  `--rep-window`, `--seed` (defaults keep the CLI bit-identical:
  temp 0 → greedy). `--fast` is ignored with a visible note when
  sampling is active (sampling advances the full state by contract).
  The console output shows the active sampler config.
- **`tests/test_engine.c`**: 32 new checks (all architectures), all
  expected values hand-computed against an independent Python
  reference (double softmax + identical xorshift64* stream), not
  fitted:
  - greedy guard (temp=0 and min_p=1) bit-identical to the native
    step over 32 steps,
  - repetition penalty breaks the repetition loop on a positive logit
    (argmax 63→62 at pen 4, T=0.001 where the softmax collapses
    exactly onto the argmax) AND multiplies a negative logit
    (model with state 14: argmax 7→6, outputs 1→7),
  - min-p surviving sets verified over fixed seeds: min_p=0.5 →
    always the argmax token (16 seeds), min_p=0.1 → only tokens
    {62,63,64} (24 seeds, ≥2 distinct), and the control without the
    filter reaches the tail (96 seeds),
  - exact xorshift64* draws for seeds 1/2/555/0 pinned to the
    hand-computed CDF bins (the seed-0→magic-state degeneracy guard
    included),
  - full 64-step seeded reproducibility,
  - argument negative controls (NULLs, vocab<2 → −3, temp>20 → −4,
    penalty<1 → −1).
- **`tools/test_count_baseline.txt`**: x86 81→113, arm 87→119 (the 32
  new checks compile on every architecture; ARM re-measured on the CI
  leg).

Measured: `./hydra-test` 113/0 (x86); baseline run
`[471, 386, 385, 386, 385, 386, 385, 386]` @ start_token=42 unchanged;
`jni-batch-test` 86/0, `large-model-test` 7/0, `gguf_test.py` 40/0.

## 2026-10-25 — M1: agent hygiene (CLAUDE.md bridge, dead artifact removal)

- **`CLAUDE.md`** (new): one-line bridge that loads `@AGENTS.md` first
  and restates the always-on rules, so Claude Code sessions and the
  repository's agent system can never drift apart.
- **`hydra_stone_model_engine.zip`** removed from git (`git rm`):
  grep over the whole tree shows it is referenced nowhere except
  session-history text — an old build artifact that bloated every
  clone. (Resolves the open decision from the 2026-10-25 audit.)
- Verified (no change needed): `.gitignore` already excludes
  `.agent_memory/` and `RESEARCH.md`, so the local research mirror and
  Notion sync notes stay out of the repository.
- The audit + derivations for this round are recorded in
  `.agents/docs/RESEARCH-LOG.md` (local fallback mirror; Notion "AI
  Notizen" sync follows at round close).
- **Not done (deliberate):** the spec title mentions "AGPL-3.0", but
  the LICENSE file is GPL-3.0 and the spec body never asks for a
  license change — changing it is a legal decision for the user, not
  an implementation task. The LICENSE file is untouched.

## 2026-10-25 — post-merge doc audit (screenshot verification pass)

PR #48 merged (20:04Z, main = afae8c8). Audit of the GitHub README
state + final docs across all folders, per user request.

### Fixed

- **License contradiction.** The LICENSE file is the full GPL-3.0
  text (GitHub's own header detects "GPL-3.0 license"), but README.md
  said "MIT — see LICENSE" in two places (License section and the
  project-structure tree) and package.json carried
  `"license": "MIT"`. All three references now say GPL-3.0. The
  LICENSE file itself is untouched — changing it is a legal decision
  for the user, not a documentation fix. (The MIT references in
  AGENTS.md / .agents/docs/ANDROID-AUDIT.md describe the pinned llama.cpp
  v0.5.0, which really is MIT, and stay as-is.)
- **Broken cross-reference in the roadmap.** The `.hydra2` item
  pointed readers to a "*Roadmap status* section" of
  `docs/ARCHITECTURE.md` — a section that never existed in that file
  (`git log -S` shows it was only ever written in the README). The
  superseded aggregated `A[i]/B[i]` design is documented under
  "Known Structural Redundancy: the Layer Loop Is Algebraically
  Free"; the reference now names that section.

### Verified (measured or checked against the tree — no change)

- Token sequence `[471, 386, 385, 386, 385, 386, 385, 386]` for the
  starter model (start_token=42, 8 steps) — reproduced with a real
  build + run.
- CI enforces the ARM64 half on every push: the `neon-test` job
  cross-compiles for aarch64, runs the suite under qemu (scalar vs
  NEON comparison happens inside the same ARM binary,
  `tests/test_engine.c` under `#ifdef __ARM_NEON`), and replays the
  parity run `42 8 --json`.
- The emulator evidence claimed for Android is present in
  `android/README.md` ("host and emulator both produced
  `[471, 386, 385, 386, …]` — bit-identical").
- `tools/make_model.py` guarantees AND asserts `A[0] != 0` and
  `B[0] != 0` (the state-blind `B[0] == 0` history is documented in
  the file).
- Web console: zero runtime npm dependencies, ESLint dev-only and
  used by the lint gate; `make ui` builds hydra-run + the starter
  model and serves on port 8787 (`server.js`).
- "4-weights-per-byte packing" roadmap item: `docs/FORMAT.md`
  "Version 2" section and `tests/test_engine.c`
  `test_version_two_format` both exist.
- ".hydra2 not implemented": no `hydra2` anywhere in `src/` or
  `include/`; `docs/FORMAT-V2.md` is the spec.
- `tools/docs_claim_check.sh`: all banned claims absent.

## 2026-10-25 — post-merge lint fix (branch `arena/6bf4fc8e-bestai`)

### Fixed

- **Android Lint findings broke the Lint & Security and Release workflows
  after the #47 merge** ("Android Lint reported findings" / "Android
  Lint must be error-free"; the gate greps the report for "No issues
  found", so even warnings fail it). Root cause: the three M5 telemetry
  strings (`run_live`, `run_done`, `run_done_cancelled`) were plain
  `<string>` resources containing `%1$d tokens` — a quantity — which is
  exactly what the `PluralsCandidate` check flags, and it violated the
  repo's own established convention (`log_size`, `log_running`,
  `log_bench_start` are all `<plurals>`; no non-plural `%d` string
  existed before this merge, which is why the gate had been green).
  Converted all three to `<plurals>` (one/other, `%1$d` positional,
  matching the existing entries) and updated the three `stringResource`
  call sites in `HydraApp.kt` to the plurals form
  (`quantity, args…`). CI compile + APK build + APK verification were
  already green on the merged commit — only the lint gate was red.
  **Round 2 (after the first CI run):** the plurals conversion removed
  all warnings (report: "3 errors, 0 warnings"), but the
  `stringResource(R.plurals…, …)` call style hit a second wall: in this
  Compose version (BOM 2024.09.03) overload resolution picks the
  `stringResource(@StringRes Int, vararg Any)` overload, so lint raised
  three ERROR-level `ResourceType` findings ("Expected resource of type
  string"). The call sites now use
  `LocalResources.current.getQuantityString(R.plurals…, …)` — one
  unambiguous signature, a `@PluralsRes` parameter, and the same pattern
  `MainActivity` already uses for its plurals. The strings stay plurals.
- **`.github/workflows/lint.yml` + `release.yml` — self-publishing lint
  reports.** The report was only readable in the Actions UI (job log +
  artifact, both on hosts outside API-only egress). Two changes make
  failures diagnosable from the API alone: (1) the `gradle lintDebug`
  step no longer aborts the job (it captures the non-zero exit — gradle
  writes the report before it aborts anyway); (2) the dedicated check
  step now publishes the full report on failure as a PR comment (via the
  issues API; the PR number comes from the event, or is resolved from
  the commit SHA via `/commits/{sha}/pulls` so push runs that update a
  PR are covered too) and to the job step summary (always), and fails
  the job itself. The first attempt (199221c) proved why the
  restructuring was needed: the publishing step sat behind the gradle
  step and was skipped on exactly the runs where the report was needed.
  Round 3 (b53d738) confirmed the restructure works (gradle step
  non-fatal, check step ran and failed the job as the sole gate), but
  the PR comment still did not arrive: the workflow token has only
  `contents: read`, so the issues API call 403s and the defensive
  `|| true` hid it. Round 4 adds job-level
  `permissions: {contents: read, issues: write}` on the android-lint
  job (lint.yml) and the build-android job (release.yml) — scoped to
  exactly the jobs that need it — plus a visible `::warning::` marker
  on the comment fallback path so a broken publish cannot hide silently
  again. Round 5 closes the last diagnostic gap: "report missing" is
  a second failure class (gradle dying before the lint analysis — e.g.
  a Kotlin compile error — produces no report file at all). The gradle
  steps in lint.yml and ci.yml now capture their full output to a
  file, and the check/publish step builds its payload from whichever
  class occurred — the lint report when it exists, the tail of the
  gradle log (150–200 lines) when it does not — published as step
  summary + PR comment in both cases. The CI "Android APK build" job
  got the same capture-and-publish treatment (plus job-level
  issues:write). From the next failed run onward, every Android
  failure in CI, Lint & Security, and Release is readable via the API.
  the lint failure artifact now also bundles the diagnostic payload
  and the gradle log. **Round 6 — root cause found and fixed:** all
  the red runs since 199221c (CI build step twice, Android lint "no
  report") were one single Kotlin compile error: the round-2 fix used
  `androidx.compose.ui.platform.LocalResources`, which does not exist
  in this Compose version (BOM 2024.09.03) — an unverified API
  assumption, used nowhere else in the repo. Call sites now use
  `LocalContext.current.resources` (LocalContext is already used in
  this codebase, e.g. Theme.kt); everything else (the plurals
  resources, the getQuantityString call style) is unchanged.

## 2026-10-25 — CI fix round (post-PR, same run)

### Fixed

- **`android/.../MainActivity.kt` — `selectEngine()` nullable receiver (CI
  compile error, `MainActivity.kt:987:57`).** The M5 edit passed
  `file.absolutePath` into `detailFor()` while `file` was still typed
  `File?`; `result.ok == true` implies a file exists, but the compiler
  cannot see through a data-derived boolean. Restructured: the no-model
  case now early-returns (identical UI state and log line as before), and
  everything after the `file == null` check runs on a smart-cast
  non-null `file` — `file.name` / `file.absolutePath` compile, and the
  success/failure UI behaviour is byte-for-byte the same. Not compilable
  in the audit sandbox (no kotlinc/NDK egress) — verified by reading;
  the CI build is the compile proof.
- **Gate scripts missing the executable bit (5 of 12 were 100644):**
  `android_autostart_negative_test.sh`, `android_crash_check.sh`,
  `android_header_check.sh`, `ci_release_version_test.sh`,
  `jni_signature_check.sh` are now 100755 in git. On a checkout whose
  umask is 000 these files appeared as `-rw-rw-rw-` (666) and could not
  be executed directly; every shipped script with a shebang now carries
  the exec bit, so no checkout can produce a non-executable script.
  All 12 `tools/*.sh` files are 100755; JS/Python tools are deliberately
  644 (invoked through `node`/`python3`, never directly).
- **CI `upload-artifact` "No files were found with the provided path:
  dist/"** — no change needed: the APK job creates `dist/` in the same
  step that inspects the built APK; that step was skipped because
  `assembleDebug` failed on the Kotlin error above. The upload runs with
  `if: always()` + `if-no-files-found: error` on purpose, so a missing
  APK is always visible. With the compile fixed, `dist/` exists again.

## 2026-10-25 — full-repository audit run (branch `arena/4a16d961-bestai`)

Six-milestone run: inventory, docs, conversion math, C/JNI hardening audit,
Android UI, docs/README truthfulness. Findings registry: `RESEARCH.md`
(git-ignored, local only) F-1 .. F-16.

### Fixed

- **`src/hydra_engine.c` — version-2 loader guards (F-1).** Header version 2
  (4 weights/byte) was accepted but had two real gaps: (a) `dim % 4 != 0`
  made the aggregation silently zero out the last `dim % 4` dimensions —
  now refused with error `-8` before any byte is read; (b) the
  `weights_len` bounds check applied the v1 size to v2 files, which would
  reject a minimal-but-valid v2 model — the check is now version-aware
  (v1: `layers × dim` bytes, v2: `layers × dim / 2` bytes), short files
  still refused with `-10`.
- **`tools/gguf_to_hydra.py` — `pick_vocab` report fix (F-3).** The vocab
  override path dropped the resolved vocabulary *count* from the
  conversion report (reported the tensor shape instead of the count the
  engine would use). Fixed; two regression checks added to
  `tools/gguf_test.py` (a dedicated override output path so the main
  conversion file used by later checks is never clobbered).
- **`tools/hydra_train.js` — pair search implemented (F-4).** The trainer's
  docstring promised a 3-rung local search (bit flip → pair swap → full
  row) but only the first rung existed, which measurably locked the search
  on some corpora (A/B in RESEARCH.md §5: official corpus 18.2% → 9.1%
  token error, alt3 27.3% → 36.4% with the old code fully locked for the
  whole run). Implemented as documented; `tools/console_train_test.js`
  still passes (it asserts properties, not a basin number).
- **`android/.../ui/HydraEngine.kt` — cancel state lost (F-14).** The run
  summary was read against a phantom `"tokens"` key (the native JSON emits
  `"emitted"`), and a cancelled run surfaced as an error. Summary is now
  built once, after `runInference`, from the native JSON; `cancelled`
  yields a completed run with a null error; dead `parseSummary` removed.
- **`README.md` + docs — density claims (F-15, F-16).** The on-disk
  density was wrong in five places (README feature line, README table +
  "honest framing" paragraph, `docs/FORMAT.md`, `docs/GGUF-IMPORT.md`
  limits table, `docs/ARCHITECTURE.md`; plus a `server.js` comment).
  Correct: v1 = 4 bits of file per weight (two 2-bit codes per byte,
  upper 4 bits reserved), v2 = 2 bits per weight (4 weights/byte, nothing
  reserved); informational density ≈ 1.58 bits/weight. Table values
  corrected (0.5 B params: v1 250 MiB, v2 125 MiB — was 125/62.5).
  The "~1.06 KiB" engine-context claim was replaced by the measured
  figure: `sizeof(HydraEngine)` = 656 B (576 B step data: two
  `int32[64]` aggregates + one `int8[64]` state). The "planned v2" wording
  became the implemented header-version-2 reference.

### Added

- **`tests/test_engine.c` — `test_version_two_format()` (10 new checks).**
  A v2 file and its byte-equivalent v1 file (hand-decoded fixtures,
  A=[1,1,0,2], B=[-2,-1,-1,-2]) must load, agree on all aggregates, and
  produce identical 40-token streams; v2 with `dim % 4 != 0` must be
  refused with `-8`; v2 with `weights_len < layers×dim/2` with `-10`.
  Falsifiability proved: corrupting one packed bit fails exactly 4 checks.
  x86 baseline 71 → 81 (`tools/test_count_baseline.txt` updated; ARM
  column derived 87 — the ARM CI leg re-measures).
- **`AGENTS.md` (root, canonical).** Architecture map (C/NEON engine,
  GGUF/llama dual engine, Python pipeline, Node/web console, Android
  dual-engine app), `.claude/skills/*` bindings, exact verification
  commands, documentation location table.
- **`docs/FORMAT.md` — "Version 2 (4 weights per byte)" section.** Layout
  (per layer: `dim/4` bytes plane w1 + `dim/4` bytes plane w2), enforced
  constraints with error codes, bounds-check semantics per version, the
  byte-parity test as proof of equivalence, and the explicit
  disambiguation: header version 2 (implemented) ≠ the `.hydra2`
  sparse-MoE container (`docs/FORMAT-V2.md`, spec-only).
- **`docs/GGUF-IMPORT.md` / `docs/FORMAT-V2.md`** — disambiguation notes
  plus the threshold proof section (the converter's `mean|W| × 0.5` cut is
  the reference absmean rule: a weight's code is nonzero exactly when
  `|w| > a/2`; literature cross-check in RESEARCH.md §4).
- **`android/.../ui/HydraApp.kt`** — `RunStats` (live tok/s, latency,
  step count), always-visible `EngineStatusRow` (`.hydra` vs `.gguf`,
  load state) and `RunTelemetryRow` (live / done / cancelled), `ModelRow`
  renders the model detail line. **`android/.../MainActivity.kt`** —
  `detailFor()` publishes llama.cpp's own quant/weights/state figures or
  the hydra format/weights/vocab; failed loads log the cause; stale
  detail is cleared when import-restore fails; live `RunStats` updated in
  `onToken`. **`values/strings.xml`** — +4 English strings.
- **`RESEARCH.md` (git-ignored)** — timestamped full file inventory,
  baseline gate results, findings F-1..F-16, b1.58/ternary-PTQ/edge-
  inference research with sources, live audit of issues #46/#44, trainer
  A/B evidence.
- **Notion page** — "📝 AI Notizen — Hydra-Stone (BestAI) Technical
  Documentation" (under the "AI-TRAINING" parent; local mirror
  `.agent_memory/NOTION_AI_NOTES.md`, git-ignored).
- **`.gitignore`** — `RESEARCH.md` and `.agent_memory/` (local agent
  workspace, never committed).
- **`CHANGELOG.md`** — this file.

### Changed

- **`tools/test_count_baseline.txt`** — x86 71 → 81 (the +10 v2 checks),
  ARM 77 → 87 (derived; CI re-measures). The drift gate fired correctly
  before the update (R21 behaviour confirmed).
- **README roadmap** — "4-weights-per-byte packing" moved to done
  (implemented as header version 2, tested).

### Audited — no change needed (verified, documented in RESEARCH.md §3)

- `src/hydra_jni.c` — fd discipline, local-ref lifecycle, no leaks.
- `src/llama_jni.cpp` — `llama_token_to_piece(…, lstrip=0, special=true)`
  matches the pinned v0.5.0 examples (verified in the submodule source).
- `tools/gpu_bench/` — exemplary "NOT MEASURED" separation + checksum
  gate; no change.
- `src/main.c`, `src/hydra_batch.h`, `src/hydra_neon.h` — clean under the
  M4 audit (leaks, fds, overflow, refs, thread safety).
- Issue #46 — scorecard numbers unverified/stale (F-6); no code action.
- Issue #44 — random-weight "VibeLUTEngine" proposal rejected per R5/R6
  (F-5); no code action.

### Blocked, not measured (stated per R32)

- ARM/NEON parity, APK build, on-device run — no aarch64 cross toolchain,
  qemu, or Android SDK egress in the audit sandbox; CI legs re-measure.
- `tools/jni_signature_check.sh` SKIPs without `javac` (gate itself green).

### Commits

- `1eaef54` — M1 (git-ignored agent workspace; RESEARCH.md)
- `202fef6` — M3 (v2 guards + tests, converter fix, trainer pair search,
  FORMAT docs)
- `c477a52` — M5 (cancel-state fix; engine status + live telemetry UI)
- (M6) — docs/README corrections, CHANGELOG, session close-out
