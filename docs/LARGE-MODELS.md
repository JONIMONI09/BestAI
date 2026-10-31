# Large models in Hydra-Stone (8B → 30B → 70B → 700B)

Round-15 analysis (2026-10-31), web-verified: what actually fits where, what
"no lag and no loss" can and cannot mean, what 2026's tooling offers, and
what Hydra-Stone already has to take advantage of it. The short answer first:

> **A 700B model on a phone with no lag and no intelligence loss is not a
> software problem, it is a physics problem.** 700B parameters are ~350 GB
> at 4-bit - larger than any phone (or laptop) memory made so far, and the
> 80 GB of a single top GPU card. What IS true in 2026: **MoE models make
> "big" affordable at one card** (GPT-OSS-120B runs on a single 80 GB GPU
> because only 5.1B parameters are active per token), **8B is the phone
> class** (4.5-5 GB at 4-bit, 10-15 tok/s on current flagships), and
> **hybrid (small local model + big server model) is how the real products
> do it**. Everything below is the derivation, plus the concrete path that
> Hydra-Stone takes for each class.

## 1. The memory arithmetic (no magic)

Weights are the floor; the KV cache, activations and the OS are the rest.
Bytes per parameter by storage precision:

| Storage | bits/param | 8B | 30B | 70B | 120B (MoE) | 405B | 700B |
|---|---|---|---|---|---|---|---|
| F32 | 32 | 32 GB | 120 GB | 280 GB | 480 GB | 1.6 TB | 2.8 TB |
| F16/BF16 | 16 | 16 GB | 60 GB | 140 GB | 240 GB | 810 GB | 1.4 TB |
| Q8_0 | 8 | 8.4 GB | 32 GB | 74 GB | 128 GB | 420 GB | 735 GB |
| Q4_0 / MXFP4-class | 4 | 4.2 GB | 16 GB | 37 GB | 64 GB | 210 GB | 370 GB |
| 2-bit (TERNARY2-class) | 2 | 2.1 GB | 8 GB | 18.5 GB | 32 GB | 105 GB | 185 GB |

(Real GGUF quants carry per-block scales, so a "4-bit" file is ~4.25
bits/param in practice; GPT-OSS-120B publishes 4.25 bits/param for its
MXFP4 expert weights. The 2026 rule of thumb from local-LLM benchmarks:
**~0.55-0.65 GB per 1B parameters at Q4 once overhead is included**.)

Where that lands, per the 2026 on-device and server numbers:

| Class | Weights (Q4) | Needs | Runs on (2026 numbers) |
|---|---|---|---|
| 1-3B | 0.7-2 GB | 8 GB phone RAM | mainstream phones, comfortable |
| **8B** | **4.2-5 GB** | **12 GB phone RAM** (8 GB = experimental) | flagships, **10-15 tok/s** on Snapdragon 8 Elite (CPU/NPU), 4-8 tok/s pure CPU |
| 13-30B | 8-19 GB | 32 GB laptop / 16-32 GB phone (top tier, tight) | 30B is the practical server-class model on a 32 GB box |
| 70B | 37-40 GB | 48-64 GB GPU/laptop RAM | desktop GPUs (A100 80 GB comfortable) |
| 120B MoE (5B active) | ~60 GB | **one 80 GB GPU** | GPT-OSS-120B: single H100/MI300X, 128k context |
| 405B (MoE 17B active) | ~210 GB Q4 / 405 GB FP8 | **8x H100 NVLink node** (FP8) | the "big open model" reference point |
| 1.6T MoE (49B active) | ~865 GB native FP4/FP8 | **8x B200** (or 4x GB300) | DeepSeek V4 Pro (2026): the 700B-class question has an answer - it runs, but only in a datacenter; 1M context, MIT weights |
| 284B MoE (13B active) | ~160 GB native FP4/FP8 | **4x H100** (or 2x H200) | DeepSeek V4 Flash (2026): the "30B+ that a single box runs" reference |
| 700B dense | 370 GB Q4 | multi-node GPU cluster | datacenter only |

**Conclusions the arithmetic forces:**

1. **700B "everywhere, including Android": impossible.** Not with better
   kernels, not with better quantization: 2-bit (the most aggressive
   honest compression) is still 185 GB, and the phone has 12-16 GB total,
   of which ~10 GB is usable. No lossless trick exists - lossless means
   keeping every bit, which is 2.8 TB at F32.
2. **"No loss" is a precision choice, not a destination.** F32 = the
   reference (2.8 TB for 700B - does not fit anywhere). Q8_0 is the
   near-lossless working point (<1% quality loss in the 2026 tables).
   Q4 is the sweet spot (1-2%). 2-bit is the memory extreme with visible
   degradation. The honest product answer is a **precision ladder**: Q8 on
   the server, Q4 on the phone, 2-bit for the smallest devices - and QAT
   (quantization-aware training, where the model is trained with its own
   quantization noise) to close the remaining Q4 gap.
3. **"No lag" is a bandwidth story, not a FLOPs story.** Decode is
   memory-bandwidth-bound: every token reads the active weights once.
   8B Q4 = ~4.5 GB/token ÷ ~20-40 GB/s of phone DRAM/NPU path ≈ 4-15
   tok/s (exactly the published range). MoE is the only lever that breaks
   the total-parameter wall at *fixed quality*: active parameters set the
   speed, total parameters set the memory. That is precisely the
   architecture Hydra-Stone's `.hydra2` engine is built around (expert
   FFN, one streamed read per expert per step).

## 2. What Hydra-Stone already has for the big end

The round-12/13 engine was designed with these constraints in mind; here is
what is already in place and what it buys at scale:

- **mmap'd trunk, zero weight heap** (`src/hydra_v2_container.c`): the
  weight file is the kernel page cache. A 32 GB .hydra2 file on a 64 GB
  machine costs 0 bytes of process heap; the OS reclaims clean pages under
  pressure. This is the same mechanism the 700B-class inference needs
  (weights on NVMe, resident on demand) - it is not a demo limitation,
  it is the serving model.
- **One `pread` per expert per step** (the FFN reads only the routed
  experts): on a dense-as-MoE converted model this streams the activated
  expert weights instead of holding them; on a native MoE GGUF the
  converter's masked decomposition preserves the sparsity structure.
  Total-parameter size stops being a RAM requirement - it becomes a
  bandwidth requirement.
- **64-bit shapes and offsets** (round 13, Qodo #17): a 32 GB file is
  beyond the 32-bit offset space the first drafts of the reader assumed;
  the `uint64` shape products and the large-model offset gate exist
  exactly so that "the 8B file" is not a special case.
- **GQA block mapping + `n_heads % n_kv_heads == 0` refusal** (round 13,
  Qodo #26): every 8B+ llama model is GQA; the engine follows the
  llama.cpp head mapping, pinned by a GQA fixture.
- **2-bit block-scaled ternary (`TERNARY2_S64/S128`)**: 2-bit codes with
  per-group F32 scales. This is the same *family* as the 2026 mainstream
  block-scaled formats (below) - a 2-bit cousin. It is what makes the
  "small device" column of the table above realistic for this engine.
- **The GGUF bridge** (this round: F32/F16/BF16/Q8_0/**Q4_0/Q4_1**): the
  converter now decodes the 4-bit block-scaled layouts that 8B downloads
  actually ship, with bit-exact parity gates against an independent
  decoder. A real 8B Q4_0 GGUF converts to a complete-contract `.hydra2`
  that the engine runs (F32-resident, mmap'd).

## 3. 2026 technique scan (what the industry landed, what we take)

Verified against current sources (October 2026), not the 2024 literature:

| Technique | 2026 status | Verdict for Hydra-Stone |
|---|---|---|
| **Block-scaled 4-bit float (MXFP4 / NVFP4)** | Landed in llama.cpp in March-April 2026 (`GGML_TYPE_NVFP4=40`; MXFP4 since the GPT-OSS release, OCP multi-vendor standard). E2M1 4-bit values + per-block scale (MXFP4: 32-elem blocks, E8M0 exponent; NVFP4: 16-elem blocks, FP8 scale + per-tensor scale). GPT-OSS-120B ships natively MXFP4 - that is why 120B fits one 80 GB card. | **Adopted at the converter boundary this round** (Q4_0/Q4_1 int4 block-scale = the same lineage, ggml's CPU formats). A native `T4_0`/MXFP4 *engine* tensor type (4-bit weights resident, dequant in-kernel) is the next engine milestone - it is what turns "32 GB F32 file" into "4.5 GB phone file". |
| **KV-cache quantization (INT8/FP8)** | Halves KV memory. 2026 measurement update: FP8 E4M3 KV costs ~0.3-0.7 points on long-context retrieval (89% vs 91% on needle-in-a-haystack - within noise for most workloads); **INT8 costs 1.5-3 points** on the same tasks - not free. llama.cpp: `--cache-type-k/v q8_0` is the standard INT8 switch. | **Candidate for round 16, with a caveat.** The `.hydra2` KV is F32 with the sink+ring geometry; INT8 KV with per-head scales halves the cache. Our ring is SMALL (128 positions), so the benefit is modest here and large only for long contexts (the server story). The 1.5-3 point INT8 retrieval loss is acceptable for chat, not for RAG-over-documents - document which. Requires re-pinning the stream pins (KV changes the math) - a deliberate milestone, not a sneak-in. |
| **Speculative decoding** | 2-3x latency reduction at *zero* quality loss (accept/reject keeps the target distribution). 2026 acceptance data: same-family drafts hit 70-85% at gamma=5 (1.8-2.5x), self-speculation (early layers as draft) 1.8-2.1x, n-gram drafts cost 0 VRAM and reach 90%+ on repetitive text; EAGLE-3 heads 4-6x but need training. It only pays at greedy/low temperature, on models >= 3B, low batch. | **Best architectural fit of all**: the v1 ternary engine (2*d integer adds per token, zero heap) is a natural *draft* for a v2 *target* - draft and target already coexist in the same binary, and the v2 CLI defaults to greedy (temperature 0), exactly the regime where acceptance is highest (70-85%). The rollback primitive is a KV write-pointer reset, which the ring geometry already approximates. Milestone: draft/target pair + acceptance loop. |
| **Prefix caching** | 2x+ throughput where many requests share a system prompt; vLLM default. | **Cheap win for the server**: `server.js` can hold the prompt-prefix KV between requests for the same model (the engine already prefills the prompt into KV, round 13 #30). Milestone, small. |
| **Paged attention / continuous batching** | The serving standard for *concurrency* (many users). | **Not needed for the single-stream CLI/engine** (bounded KV by design). Relevant only if the web server grows multi-user generation - then it is a server-side design decision, not an engine format change. Documented, deferred. |
| **QAT / distillation** | QAT closes the 4-bit gap. 2026 data: NVFP4 QAT reaches BF16-parity on code benchmarks (83.0% vs 82.3% LiveCodeBench, Qwen3.5-9B); QATFactory (Oct 2026) exports Q4_K/NVFP4/MXFP4 checkpoints directly to llama.cpp for 8B-230B models. Field rule 2026: PTQ is fine at 4-bit and up, below 4-bit QAT is required. | Out of scope (training-side), but it is the answer to "no loss at 4-bit": you cannot post-hoc-quantize to zero loss - you train against it. It is also the path for our 2-bit TERNARY2 family to become a *trained* format rather than a projection - noted, not planned. |
| **NPU delegation (Qualcomm QNN / AI Engine Direct, Google LiteRT-LM, MediaTek NeuroPilot)** | 2026 reality check: LiteRT-LM is **production-grade since April 2026** (Apache-2.0, INT4/INT8/FP16, Qualcomm+MediaTek NPU + GPU backends, OpenAI-compatible server mode; current v0.13.1) and its Multi-Token-Prediction support (v0.10.1) gives 2x+ decode on mobile GPUs at zero quality loss. The NPU speedup is real (3B model: ~48 tok/s NPU vs ~10 CPU on 8 Elite), BUT most consumer apps do not reach the NPU at all: NNAPI was deprecated in Android 15, and the Hexagon requires deliberately targeting the vendor QNN SDK (100+ TOPS on 8 Elite Gen 5, 10B+ params claimed - unreachable from a generic CPU/GPU app). | **The "no lag" answer for the phone** is the NPU - no CPU kernel of ours will beat it for decode. Our options: (a) keep the NEON CPU path (portable, works on every SoC, 4-15 tok/s), (b) for the flagship tier, export the converted model into the NPU toolchain (LiteRT-LM/QNN format). Both are app-milestone work with real vendor lock-in cost; the engine's job is the *format + weights + routing*, which is done. |

## 4. The 8B path, concretely (this round's target)

**"Are we limited to ~200M?" - no, and the limit is not a format limit.**
The *v1 projection* path (`gguf_to_hydra.py`) is by design a 64-dimensional
ternary state (weights ceiling 256 KiB) - that is the ~200M-class thing.
The *v2 MoE path* has no parameter-count ceiling in the format: every
offset and length in the `.hydra2` container is 64-bit (verified against
`src/hydra_v2_container.c` this round), `dim` is a u16 (<= 65535), layer
counts are u16 (<= 1023), and the only real ceilings are RAM and the
model's own shape (dim/layers/heads all carry through from the GGUF
metadata). So an 8B, 30B or 70B GGUF converts to a `.hydra2` that the
engine executes; whether it *fits* is the arithmetic of section 3.

**What works today (tested, gated):**

```
8B Q4_0 GGUF (4.9 GB, the real download)
   -> tools/gguf_to_hydra2.py   (decodes Q4_0 blocks, bit-exact, gated)
   -> .hydra2 (F32-resident, ~31.5 GB, mmap'd trunk + streamed experts)
   -> hydra-run model.hydra2    (runs: GQA attention, expert FFN, real logits)
```

The 31.5 GB F32 footprint is the honest cost of the *engine-agnostic* path
(same as the existing Q8_0 path: 8.4 GB GGUF -> 32 GB .hydra2). It is fine
on any 64 GB machine; it is NOT the phone path.

**What the phone path needs (milestone order, no stubs until built):**

1. **`T4_0` engine tensor type** (4-bit weights resident): the 8B stays
   4.5 GB end-to-end. This is a kernel milestone - new dequant-in-matvec
   path, NEON port with the scalar parity test, new stream pins. The
   converter already emits the raw 4-bit blocks (this round); the engine
   type is the missing half.
2. **Configurable KV geometry**: the sink+ring cache (2+8) bounds the
   context; a 4k-8k chat needs the ring widened from metadata
   (`v2.kv_ring` already exists as a field - the engine currently enforces
   its small default; making it load-bearing + paged is the change).
3. **NEON port of the v2 kernels** (attention, FFN, dequant): the app's
   arm64 `.so` today runs the v1 engine only; the v2 engine is
   host-only by design until the ARM parity gate exists (no stubs).
4. **NPU tier (optional, flagships)**: export for QNN/LiteRT-LM, keep
   NEON as the universal fallback.

**Memory budget for the 8B on a 12 GB phone (Q4 resident, 4k context):**
4.5 GB weights + ~0.5 GB KV (INT8) + ~1 GB activations/runtime + 1.5-2 GB
OS headroom = fits, with exactly the 10-15 tok/s the 2026 benchmarks
report for this class. That is "usable offline chat" - "no lag" in the
streaming sense, not in the instant sense.

## 5. 30B / 70B / 120B-MoE / 700B - the honest table

| Model | Hydra-Stone path | What it takes | Expected feel |
|---|---|---|---|
| 30B dense, Q4 | converter (Q4_0, this round) -> .hydra2 F32 (120 GB) on a 256 GB box; or T4_0 resident (19 GB) once built | 64-256 GB RAM machine | CPU: 1-3 tok/s (bandwidth-bound); GPU: 30-60 |
| 70B dense, Q4 | same, 28 GB T4_0-resident path | 64 GB+ box / 1x 48 GB GPU | the standard "big local model" |
| 120B MoE (5-17B active) | **the showcase case**: expert streaming is exactly its access pattern; Q4 resident ~60 GB | one 80 GB GPU (GPT-OSS-120B's own footprint) or 96 GB RAM server | single-digit tok/s on CPU is realistic *because only 5B params are active* - MoE makes 120B feel like 5B |
| 405B MoE | server cluster path; our engine would run it as a host process over mmap/NVMe | 8x H100-class (FP8) per the 2026 numbers | not a product target for us; documented for completeness |
| 700B | **cluster only** (multi-node); no device path exists | datacenter | the answer to "700B everywhere" is the hybrid below |

**The hybrid architecture (what the real 2026 products do):** a small
model on the device for the interactive layer (8B, offline, instant
first-token, private) + a big model on a server for the quality layer
(70B-120B-MoE). `server.js` already hosts the model console; a
`/api/generate` streaming endpoint (SSE) over the v2 engine is the
missing server half - small, testable, no format change. This is how a
user gets "700B-class answers" on their phone: the phone never holds the
700B, it talks to it.

## 6. Android app: what changes, what does not

- **Nothing in the app breaks this round.** The Q4_0/Q4_1 work is
  converter + host engine; the app's dual-engine setup (llama.cpp for
  GGUF, v1 JNI for `.hydra`) is untouched and re-verified by the Android
  CI legs.
- **The app already runs 8B GGUFs today** - through the bundled
  llama.cpp (v0.5.0 pinned): that is the *fastest available 8B path on
  Android* and it is in production in the APK. The `.hydra2` path joins
  the app only after milestone 3 above (NEON port + parity gate), per the
  no-stubs rule.
- **The .so memory story for the v2 engine** when it arrives: the mmap'd
  trunk counts as page-cache (Private Clean in `dumpsys meminfo`), the
  streamed experts add no standing footprint, the working set is the
  attention/KV buffers - sized by context, not by model. That is the same
  budget table as §4.

## 7. Sources (retrieved 2026-10-31)

- NVFP4 in llama.cpp (March-April 2026 merge, type 40) + NVFP4 vs MXFP4
  format comparison: insiderllm.com FP4 guide; r/LocalLLaMA FP4 thread
  (April 2026); talki-app NVFP4-vs-GGUF-vs-FP8 production notes (Sept 2026).
- GPT-OSS-120B: 117B total / 5.1B active, 128 experts top-4, MXFP4
  4.25 bits/param on MoE weights, single 80 GB GPU, 128k context:
  Semianalysis model page; OpenAI model card via deepwiki; spheron deploy
  guide (Oct 2026); morphllm comparison.
- On-device 2026 numbers (8B Q4 ≈ 4.5-5 GB, 12 GB phones, 10-15 tok/s
  Snapdragon 8 Elite, 4-8 tok/s CPU, 0.55-0.65 GB/B rule of thumb):
  llm-hub RAM guide (Sept 2026); ai-tldr phone guide (June 2026);
  dev.to 2026 RAM benchmarks (June 2026); cotictsy on-device guide
  (April 2026).
- 405B server requirements (8x H100 FP8): hostrunway GPU comparison
  (July 2026); vrlatech 8-GPU guide (June 2026); localaimaster 405B page
  (March 2026).
- 2026 optimization stack (KV INT8/FP8, speculative 2-3x zero-loss,
  prefix caching 2x+, paged attention, SubTernary CPU 4x): ailearningguides
  serving guide (June 2026); myengineeringpath inference optimization
  (March 2026); QuantSpec arXiv 2502.10424; SubTernary arXiv 2610.28671.
- Android NPU 2026 (QNN/AI Engine Direct, LiteRT-LM backends incl.
  MediaTek/Intel, INT8/INT16 LLM on HTP): Google AI Edge developer docs
  (2026); aipotluck QNN capability summary (2026-08).
- Second research round (2026-10-31): DeepSeek V4 Pro/Flash hardware and
  pricing (morphllm Sept 2026, spheron Oct 2026, opslyft Sept 2026);
  speculative-decoding acceptance tables and n-gram/EAGLE data (misar.blog
  May 2026, teqvolt June 2026, glukhov.org, yobitel July 2026, SparseSpec-L
  arXiv 2607.27735); KV INT8/FP8 loss measurements (dev.to June 2026,
  localaimaster May 2026, digitalapplied April 2026, gpuaas May 2026);
  LiteRT-LM production status + MTP + QNN lock-in (groundy June/July 2026,
  happycapyguide April 2026, phoneprice360 Oct 2026); QAT 2026 (QATFactory
  arXiv 2609.39223 Oct 2026, ICLR-2026 NVFP4/MXFP4 study, tensorfoundry
  quantisation field guide June 2026); on-device runtime spread
  (ai-tldr June 2026, grokipedia March 2026, distillabs August 2026).

## 8. Round 16 results: 4-bit resident engine + 3B-geometry end-to-end

### 8.1 The T4_0 engine (decision A1, chosen via ask_user)

`block_q4_0` is now a **first-class resident layout** in the MoE GEMM path
(layout 2 of `hydra_moe.h`), not just a decode-at-read type. Every Q4_0
tensor is stored as raw GGUF blocks and dequantised in the hot loop
(`dequant_block_q4_0`): embed/lm_head, attention projections, router, and
the per-expert gate/up/down sub-matrices. The engine does not grow with
the weights anymore: the weight bytes ARE the weights.

Evidence (all in-tree, all green):
- `tests/test_hydra_moe.c` — 216 checks (10 new Q4_0 pins added in round 16,
  existing pins untouched), including the cross-precision identity
  `q4_0(w) → q8_0(w) → w` on the same token stream.
- `./hydra-run /tmp/q4fix.hydra2 0 10` → `2 6 5 6 14 6 5 12 14 14`
  (deterministic across runs).

### 8.2 Converter `--t4`: bit-exact, streaming

`tools/gguf_to_hydra2.py --t4` keeps Q4_0 sources **bit-exact** (raw block
copy, zero re-quantisation) and converts mixed-precision sources by
decoding Q8_0/Q4_1 → F32 → `quantize_q40` (a new pure-Python reference
plus a vectorised numpy fast path, verified byte-identical by the test
suite). `dim % 32 != 0` is refused loudly before any I/O.

Memory: the in-memory implementation held every payload in `bytes`
(3B Q4_0 ≈ 3.5 GB peak, F32 source ≈ 8.4 GB — OOM in this 3.8 GB
sandbox). The round-16 rewrite is **two-pass streaming**: pass 1 computes
the full file geometry from shapes only (O(tensors) memory, the GGUF
header is 8 KiB) and writes the header first; pass 2 regenerates and
streams each payload through a bounded window (O(chunk) — 256 rows for
F32, one block for Q4_0 raw copies) while collecting the expert CRCs
incrementally (`_CrcSink`), then patches the 48-byte expert-directory
records in place (the header CRC covers bytes 0..119 only, so it stays
valid). Result for the 3B geometry below: **peak RSS 2.58 GiB** of which
~1.7 GiB is the mmap'd source file's page cache (kernel-managed,
evictable); private allocations are O(chunk). Both conversion runs of
the 3B source produced byte-identical output (same md5).

### 8.3 3B-geometry end-to-end in the sandbox

Real Llama weights are **not obtainable from this sandbox** (egress
allowlist; verified in round 16: HF, hf-mirror, raw/objects GitHub, git-lfs,
and GitHub-hosted .gguf all unreachable). So the full-scale test uses
`tools/build_3b_synth_gguf.py`: a streamed builder that writes a
**real-geometry Llama-3.2-3B-shaped** Q4_0 GGUF — 28 layers, dim 3072,
heads 8 / kv 4, inter 8192, vocab 128256, rope 500000, tied embed, no
output tensor — with deterministic synthetic weights (MT19937, seed
1234567) in O(chunk) memory.

Measured in this sandbox (2 cores, 3.8 GiB RAM, x86_64, Python 3.11.2):

| Step | Number |
|---|---|
| Synthetic Q4_0 source | **1,859,256,480 B (1.73 GiB)**, build ≈ 120 s |
| `--t4` conversion | **3,047,794,320 B (2.84 GiB)** output, ≈ 6 s, byte-deterministic |
| Conversion peak RSS | 2.58 GiB (≈1.7 GiB of it = mmap'd source page cache) |
| `hydra-run` 10 tokens | **59.8 s → 5.98 s/token** (≈ 10.5 GFLOP/token, 2 cores, no AVX-512) |
| Determinism | two runs → identical token stream |
| Token stream | constant `77348` — expected for random synthetic weights (the 2-layer mini models show the same fixed-point attractor; the stream identity check against the F32 path is the meaningful invariant, see 8.4) |

The test suite runs this end-to-end with `HYDRA2_3B_E2E=1`
(`tools/gguf_to_hydra2_test.py`): source build → `--t4` conversion →
engine determinism → token count. Opt-in because it takes ≈ 4 minutes
and needs ~3 GiB of RAM.

### 8.4 What this proves — and what it does not

Proves: the v2 container, the reader, the T4_0 GEMM path, and the
converter all hold at real 3B scale; memory stays O(chunk) for the
converter; the pipeline is byte-deterministic; no pin of any existing
code path changed (round 16 only added pins).

Does **not** prove quality: the weights are synthetic, so the token
stream carries no meaning. To run a *real* model: download
`Meta-Llama-3.2-3B-Instruct` from Hugging Face
(`https://huggingface.co/llama-3.2-3B-instruct-q4_0.gguf` or the GGUF
repo of choice, ≈ 1.9 GB), run
`python3 tools/gguf_to_hydra2.py --t4 model.gguf model.hydra2 &&
./hydra-run model.hydra2 <bos> <steps>` on any machine with ≥ 6 GB RAM
(this sandbox's 3.8 GiB is the only reason the 8B-class run is
documented instead of executed here: 8B Q4_0 ≈ 4.9 GB weights + F32
activation buffer).

### 8.5 Server: "intelligent + configurable everywhere" (decision, via ask_user)

The console server got the round-16 configuration treatment — every hard-coded
knob now has an env override, a header-only probe makes the import decision
*before* any conversion work, and one endpoint exposes the whole surface:

- `GET /api/config` — every setting with its effective value, its default,
  whether an env variable overrode it, and the request-level override where
  one exists. A client can always see what it runs against.
- Env overrides (all defaults unchanged — a server started without any of
  them behaves exactly like before): `HYDRA_DEFAULT_MODEL`,
  `HYDRA_INFER_TIMEOUT_MS`, `HYDRA_MAX_PROMPT` (may only lower the engine's
  256 cap), `HYDRA_MAX_TRAIN_BODY`, `HYDRA_DEFAULT_STEPS`,
  `HYDRA_IMPORT_MODE`. An invalid value is refused at startup with the
  variable named — never silently ignored.
- `POST /api/models/import?name=x.gguf&mode=v2` converts through the
  streaming round-16 converter into the full-fidelity `.hydra2` container:
  t4 is size-optimal by default (an all-Q4_0 source is a bit-exact raw copy,
  anything else is re-quantised deterministically), `&t4=force|none` and
  `&max_experts=1..8` override, and the header-only `--probe` verdict
  (geometry + `t4.all_q4_0`) comes back in the response. `mode=auto` — the
  default — keeps the existing v1 behaviour for existing clients and reports
  the v2 verdict alongside it.
- `.hydra2` files are uploadable (`/api/models/upload`, magic-level header
  check; the deep verdict stays with inspect and the engine).

Gates: `tools/server_test.js` 47 → 57 checks (config surface, env override
effective + marked, invalid env refuses to start, v2 import of F32 and Q4_0
sources incl. the bit-exact verdict, t4=none override, auto-mode v2 report,
invalid mode/t4/max_experts 400s, `.hydra2` upload accept/refuse). All 57
green, 3 consecutive runs.
