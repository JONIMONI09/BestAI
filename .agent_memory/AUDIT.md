# Hydra-Stone — Evidence-Based Technical Audit (2026-10-26)

Method: every claim below was verified against the source at this commit
(read + executed), not against README wording. All test runs in this
document happened locally on 2026-10-26 (Debian 12 x86-64, gcc 12.2.0)
unless marked CI. File:line citations use the state at audit time.

## 1. WHAT ACTUALLY WORKS (verified strengths)

**Zero-heap engine — TRUE, with one honest caveat.**
- `grep -n 'malloc|calloc|realloc|strdup|alloca' src/hydra_engine.c` →
  **zero** allocator calls (the only hit is the comment at line 652).
  The inference path (`hydra_engine_step`, `src/hydra_engine.c:501`)
  uses a stack-local `int64_t accumulator[64]` (line 521) and the
  fixed-size struct.
- Caveat: `src/main.c:479` **does** `malloc(sizeof(uint16_t) * steps)`
  for the CLI token buffer — bounded by the clamp at `src/main.c:461`
  (`steps > 256 → 256`, i.e. ≤ 512 B). So "zero-heap inference" is
  true for the engine library; the CLI allocates a small, capped buffer.
  The README scopes its claim to `src/hydra_engine.c` (line 161) —
  accurate, not an overclaim.

**Fixed-size context — TRUE, corrected to 664 B.**
- Measured: `sizeof(HydraEngine) = 664` (x86-64, gcc; pinned at compile
  time by the new `hydra_engine_size_check` typedef in
  `include/hydra_model.h:81`). The README's 656 B was stale (pre-
  `rng_state`). 576 B of that is the per-token working set
  (agg_a 256 + agg_b 256 + state 64).

**mmap loading — TRUE.** `hydra_engine_load`
(`src/hydra_engine.c:333`): `mmap(PROT_READ, MAP_SHARED)`,
`madvise(MADV_SEQUENTIAL)` (line 353), single aggregation pass at load,
zero weight re-reads per token.

**Header validation — STRONG.** Four sequential checks
(`src/hydra_engine.c:375-447`): magic → version (1 or 2) → dim/vocab
bounds (1..64 / 1..1024) → v2 `dim % 4 == 0` → `layers ≤ 4096`
(signed-overflow PoC documented in the comment) → `weights_offset ≥ 24`
→ `offset + len ≤ file_size` (uint64 arithmetic) → required-bytes check
per version (`layers*dim` v1 / `layers*dim/2` v2). Crafted-header PoCs
are covered by `tests/test_large_model.c` (7/7 green, incl. "offset+
length wrapping past 4 GiB is refused").

**SIMD parity — TESTED, not assumed.** `tests/test_engine.c`:
`test_aggregation_matches_layer_loop` (line 311),
`test_fast_step_matches_full_step` (363), `test_neon_matches_scalar`
(841, runs on AArch64 builds incl. CI neon-test),
`test_avx2_step_kernel` (1765), `test_avx2_v2_unpack` (1826),
`test_neon_v2_load_matches_scalar` (975). Suite: 142/0 failures;
jni-batch 86/0; large-model 7/0; count-vs-baseline ok.
`make test-run`, `python3 tools/gguf_test.py` (59/0) and
`node --test tools/server_test.js` (37/37) all pass locally right now.

**server.js — genuinely hardened** (`server.js:32-43,77,487-492`):
`execFile` with argument arrays (no shell), engine timeout + 4 MiB
maxBuffer, real cancellation via child kill on `req.close`,
`realpath` + `lstat` path guard against symlink escape (`server.js:141-154`),
`randomUUID` upload names, 2 MiB body / 64 MiB upload caps,
default bind `127.0.0.1` (remote only with explicit
`HYDRA_ALLOW_REMOTE=1`, line 77).

**Static analysis (run 2026-10-26, this audit):** gcc
`-std=c99 -Wall -Wextra -Werror -fanalyzer` rc=0; cppcheck
`--enable=all` on src/+include/: 0 error, 0 warning, 10 style — all 10
are exported APIs (`hydra_rmsnorm`, `hydra_rope_inplace`,
`hydra_swiglu`, `hydra_kv_cache_*`, `hydra_simd_has_avx2`,
`hydra_kv_cache_size`, `hydra_engine_resident_kb`) whose only callers
are in `tests/` (outside the scan scope) — 0 real defects;
clang-tidy `bugprone-*,cert-*,concurrency-*,clang-analyzer-*`: 0 real
analyzer bugs, 0 concurrency findings (155 findings, all classified
intentional: Annex-K `_s` suggestions not implementable in glibc,
stderr fprintf return values, NEON signed-bitwise idioms, the required
feature-test-macro defines); flawfinder `--minlevel=4`: **no hits**.

## 2. ARCHITECTURAL ILLUSIONS & MATHEMATICAL LIMITS

**The layer aggregation is an exact reduction, not an approximation —
and that is the ceiling.** `hydra_build_aggregation`
(`src/hydra_engine.c:160-240`) computes
`A[i] = Σ_l w1[l][i]`, `B[i] = Σ_l w2[l][i]`. This is algebraically
exact because within one step `token_val` and `state[i]` are constant
across layers (documented at `src/hydra_engine.c:45-53` and proven by
`test_aggregation_matches_layer_loop`). Consequently a 4096-layer
model is, after load, **the pair of 64-int arrays A and B** — nothing
else. There is no nonlinearity *between* layers (the clamp in
`hydra_engine_step`, lines 553-557, is applied once per step, after the
sum), so L "layers" of linear maps compose into one linear map.
**Proof of expressive power:** the entire state of a loaded model,
regardless of layers/dim/vocab within v1 limits, is
`A[0..63], B[0..63] ∈ {-4096..4096}` = **128 integers ≈ 1664 bits
(≈208 bytes)** of model capacity. A 4096×64×2-weight file (262 144 B,
524 288 ternary weights) compresses losslessly to those 128 numbers —
the format's own `docs/FORMAT-V2.md` (roadmap table) records this as
the reason an "aggregated container" design was superseded.

**Column-0 bottleneck — confirmed in code.** The output token of
`hydra_engine_step` is
`((acc[0] % vocab) + token_in + 1) % vocab` (line 568) — only
`accumulator[0]` is read. Since `acc[i]` depends only on `state[i]`
(line 553-557, per-dimension clamp), dimension 0's trajectory
`(token_t, state[0]_t)` is closed: it never needs dimensions 1..63.
That is exactly what `hydra_engine_step_fast` exploits
(`src/hydra_engine.c:602`): one multiply-accumulate per token,
**bit-identical token stream** (contract documented in
`include/hydra_model.h:83-95`; staleness of state[1..63] after a fast
step is tested). Dimensions 1..63 exist in the full path only as
write-only state. The sampler shares the same limit: its logit is
`agg_a[0]*c + agg_b[0]*state[0]` (`src/hydra_engine.c:641-648`
comment block) — min-p sampling over a **1-dimensional** score
function.

**What a real LLM loses in conversion** (`tools/gguf_to_hydra.py`):
ternary quantization `W̃ = clip(round(W/mean|W|), -1, 1)` (line 39),
`MAX_DIM = 64` (line 64), `MAX_VOCAB = 1024` (line 65). A 7B model
(≈7·10⁹ weights, hidden ≥ 2048, vocab ≥ 32k) can therefore keep at most
524 288 ternary weights (**0.0075 %** of the parameters), projected
into a 64-wide state, and the decoder only ever reads one dimension of
it. The converter's own header comments state this plainly (lines
281/304: "be folded into the 64-wide state"; line 318: "The first
MAX_VOCAB pieces are what fit"). The intelligence loss is total by
construction — nothing in `hydra_engine_step` can recover rank beyond
what 128 integers hold. This is a **documented design limit of the
ternary-recurrence format**, not a bug in the implementation; the
honest engineering statement is: `.hydra` v1 models are compact
deterministic token automata with at most a 255-state memory
(state[0] ∈ int8), not LLMs.

## 3. DEAD CODE & UNFINISHED ROADMAP

**M4 transformer primitives: exist, unit-tested, NOT wired.**
`hydra_rmsnorm` (`src/hydra_engine.c:664`), `hydra_rope_inplace`
(685), `hydra_swiglu` (717), `hydra_kv_cache_init/append/size/get/
sink` (750-830): 38 references in `tests/test_engine.c`
(`test_rmsnorm` 1905, `test_rope` 1953, `test_swiglu` 1994,
`test_kv_cache` 2060), **zero references in production code**
(`src/main.c`, `server.js`, `tools/` — grep-verified). They are float
functions; the engine's hot path is int8/int32 ternary recurrence with
no attention, no FFN, no float arithmetic. They are building blocks,
not a hidden transformer (cppcheck's 10 "unused" style hits are
precisely this set).

**`.hydra2` (FORMAT-V2.md): spec-only, zero implementation — honestly
labeled.** The doc's own first lines: "Status: SPECIFICATION. There is
no v2 loader in this repository. src/hydra_v2_reader.c does not exist."
No gap between doc and reality there. **However, the naming collision
is real and dangerous**: `version == 2` inside the 24-byte v1 header
**is** implemented (2-bit/4-weights-per-byte packing,
`hydra_build_aggregation_v1_1`, `src/hydra_engine.c:181`, loaded via
the `version == 2` branch at line 379) — that is a v1.1 *packing*
variant, not the sparse-MoE `.hydra2` container. `docs/FORMAT-V2.md`
disambiguates this explicitly (roadmap table), which is good; any
reader who skips the disambiguation box will believe the MoE container
exists.

**`HYDRA_DROP_CACHE`** (`src/hydra_engine.c:580`): build-flagged
`madvise(MADV_DONTNEED)` per step — documented as measured-and-rejected
for default use; fine as opt-in.

## 4. SECURITY, EDGE CASES & CODE SMELLS

**Verified clean (this audit's runs):** no format-string sinks with
user data (all `fprintf`/`perror` use literal formats); no `strcpy/
strcat/sprintf` in `src/` (flawfinder minlevel 4: no hits);
integer overflow on the size paths uses `uint64_t` accumulation
(`src/hydra_engine.c:430-447`); the layer cap exists because a
16.9-GB-header PoC would overflow the int32 accumulator (documented,
line 410-415); upload paths in `server.js` are UUID-named, capped,
realpath-verified; engine CLI `steps` is clamped (line 461).

**Remaining hazards (ranked):**
1. **mmap TOCTOU → SIGBUS** (by-design risk): the file is
   `fstat`ed then mapped then read at load. If the file is truncated
   between `fstat` and the aggregation read, a page beyond the new
   EOF faults with SIGBUS (process death, not a clean error). The
   validation prevents *header-claimed* OOB reads (PoC-tested), but a
   racing truncation is not catchable in user space. Impact: local CLI
   + console only (models are local files, no network exposure of the
   engine path). Accepted risk; mitigation would be readv-based
   loading, which abandons the mmap design.
2. **`-D__APPLE__`/portability surface** (fixed this round,
   see RESEARCH.md §10): unconditional in-file `_POSIX_C_SOURCE`
   hid `O_NOFOLLOW`/`MADV_SEQUENTIAL` on Darwin; now Apple-conditional
   with a regression test + macOS PR-tier scan job.
3. **README size claim drift** (fixed this round): 656 B → 664 B,
   now compile-time pinned (`hydra_engine_size_check`).
4. **Clang-tidy reserved-identifier noise** (accepted): the in-file
   feature-test defines (`_POSIX_C_SOURCE`, `_DEFAULT_SOURCE`) trip
   `bugprone-reserved-identifier`/`cert-dcl37-c` (4 findings per file
   pair) — the defines are the sanctioned self-containment mechanism
   and cannot be avoided without re-introducing the "every build must
   remember -D flags" fragility.
5. **`--fast` staleness contract** (documented, tested): after
   `hydra_engine_step_fast`, `state[1..dim-1]` is stale
   (`include/hydra_model.h:83-95`). A future consumer mixing fast and
   full steps in one session would read stale state — the header
   contract + test guard this, but it is a trap for anyone adding a
   new frontend.
6. **32-bit ARM** (known, documented): NEON v2 unpack uses
   `vqtbl1q_u8` (AArch64-only) — 32-bit ARM (armeabi-v7a, built by
   CI's android-apk) takes the scalar decode (`src/hydra_engine.c:215-235`
   comment). Correct, but one step closer to the AArch64-only path
   for every new SIMD kernel added.

## 5. PRIORITIZED FIX & EVOLUTION PLAN

Goal: from "1D linear toy recurrence with enterprise-grade CI gates"
to a genuine (ternary-capable) LLM inference engine. Ordered by
capability-per-effort, each step testable with the existing gate
machinery:

**P0 — honesty layer (days):**
1. README/ARCHITECTURE: state the §2 limits in one boxed paragraph
   (128-integer effective capacity; column-0 decode; 255-state memory;
   converter's 0.0075 % figure). The code is honest; the front door is not.
2. Keep the 664 B compile-time pin; add the same pattern to
   `HydraModelHeader` (24 B) so the on-disk contract is pinned too.

**P1 — break the 1D collapse (weeks, format v1.2, backward compat):**
3. Add a **per-step cross-dimension mix** to the recurrence:
   `s_{t+1}[i] = clamp(A[i]·x_t + B[i]·s_t[i] + M[i][j]·s_t[j] term)` —
   i.e. replace the scalar B[i] with a small per-layer-mixed
   `int8[64][k]` mixing matrix (k = 2..8 sparse rows, still ≤ 4096
   magnitude, still int32-exact under the existing cap). This is the
   minimal change that makes `step_fast` *wrong* (as it should be) and
   gives the state real capacity. Requires: new header field or
   version 3, `test_mixed_step_reproduces_unmixed` (k=1 must equal
   today's output — parity test in the existing style), NEON/AVX2
   kernels with scalar-parity tests (the repo already has this
   discipline: §1 SIMD parity).
4. Move decoding off column 0: token logits from the **full** state
   (`L(c) = Σ_i W_out[c][i]·s_t[i] + b[c]`, int8×int8→int32, 64 MACs
   per candidate — cheap), keeping `--fast` as the explicit
   column-0-only mode with its existing staleness contract.
   The M4 `hydra_kv_cache_t` shows the repo already knows the
   zero-heap stack-array pattern for bounded vocab (8 KiB at 1024).

**P2 — wire the transformer (months, this is what M4 was for):**
5. Implement the `.hydra2` sparse-MoE container **exactly as specified**
   in `docs/FORMAT-V2.md` (128-byte header, experts, top_k routing,
   streaming) with `src/hydra_v2_reader.c`. The spec is canonical and
   complete; the reader is the missing ~100 % of it. The M4 primitives
   (rmsnorm/rope/swiglu/kv-cache, all unit-tested) are the building
   blocks; the missing piece is an attention block that calls them —
   and float arithmetic in the hot path, which the current
   int8-exact-integer design deliberately avoids. Decision to record:
   float attention kernels + int8 state interface, with parity tests
   against a reference implementation on fixed corpora.
6. Raise the converter ceiling *only after* P1/P2: a ternary MoE
   target keeps the format's identity while its capacity grows with
   experts × dim instead of 128 integers.

**P3 — hardening (continuous):**
7. SIGBUS TOCTOU: add `MAP_POPULATE` as a load-time opt-in flag
   (faults happen during load where a clean error is possible) for
   environments where racing truncation is a real threat.
8. Pin `HydraModelHeader` (P0.2) and extend `tools/apple_portability_
   check.sh`-style compile-time pins to any other ABI-visible struct.

Not doing: replacing the mmap zero-heap design (it works, it is
measured, and the §4.1 TOCTOU is the only cost); "fixing" the
clang-tidy reserved-identifier findings (the defines are load-bearing);
deleting the M4 primitives (they are the P2 inventory).
