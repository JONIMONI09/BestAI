# Hydra-2 Research: Best Math, Bounded Memory

> **Status: RESEARCH SPECIFICATION. No engine code, no measurements.**
> Nothing in this document has been built. Every number is either measured and
> attributed, projected and labelled, or explicitly **NOT MEASURED**. Where this
> document says a benchmark will decide something, the benchmark has not been
> run and the question is open.
>
> The canonical format specification is [`FORMAT-V2.md`](FORMAT-V2.md). This
> document explains *why* that format, and what would have to be true for it to
> be worth building.

---

## 0. Provenance rule for this document

The same labels as `FORMAT-V2.md`:

| Label | Meaning |
|---|---|
| **[CODE]** | A symbol in this repository a reader can check. File and line given. |
| **[SOURCE]** | A statement from a named external primary source, with a link. |
| **[MEASURED]** | A measurement, with machine and date named. |
| **[ESTIMATE]** | An explicit projection with its assumptions written out. |
| **[PLANNED]** | A measurement or experiment that has **not** been run. |
| **[NOT MEASURED]** | A quantity that must not be presented as if it had been. |

---

## 1. Terminology: bounded, measured memory

### 1.1 The phrase being retired

Earlier drafts of this project **previously claimed** a "zero RAM" goal. That
phrase was an unproven claim and has been removed — `tools/docs_claim_check.sh`
now fails the build on it, and this paragraph survives only because the line
above carries the marker the gate's allow-list looks for.

It is unsatisfiable for a physical reason, not a rhetorical one:
mapping a file makes its pages real physical memory the moment they are touched,
and the kernel — not the application — decides when they leave. An application
cannot honestly promise a resident set of zero.

What replaces it:

> **Bounded, measured memory** — the resident cost of the process is a *number
> we can state, explain and reproduce on a named device*, not a slogan. Every
> claim about memory in this project must come with the device it was taken on,
> or be labelled NOT MEASURED.

### 1.2 Peak-PSS, defined

Three different numbers get called "memory used" and they answer different
questions. This project uses **Peak-PSS** as the headline figure, defined as:

```
Peak-PSS  =  max over time of  Pss Total

Pss Total = Private Dirty + Private Clean + (this process's equitable share of
             memory shared with other processes, e.g. the Zygote boot image)
```

- Android's own documentation calls PSS "the best metric for *how much memory is
  this app responsible for*" and splits it into **Private Dirty** (only this
  process, modified — the number that indicates a leak, because it cannot be
  dropped) and **Private Clean** (only this process, an unmodified copy of a file
  on storage — *droppable by the OS under pressure*). **[SOURCE]**
- **Why Peak and not a point sample.** The interesting memory moment is the peak
  during load, not the value after the app has settled — a model that is loaded
  and then released never shows its cost in a single end-state reading.
- **Why PSS and not RSS.** RSS counts every resident page including pages shared
  with every other process on the device, so it overstates responsibility for a
  zygote-shared mapping. It is still recorded as a secondary number because a
  drop in RSS does not mean a drop in system pressure (see §2.2).

### 1.3 What the engine already costs **[CODE]**

Measured on this repository, from the source and from a compiled program that
prints the sizes (`include/hydra_model.h`, `src/hydra_engine.c`):

| Buffer | Type | Size |
|---|---|---|
| `HydraEngine.state_vector` | `int8_t[64]` | 64 B |
| `HydraEngine.agg_a` | `int32_t[64]` | 256 B |
| `HydraEngine.agg_b` | `int32_t[64]` | 256 B |
| per-step accumulator in `hydra_engine_step()` | `int64_t[64]` | 512 B |
| **total resident while stepping** | | **1088 B ≈ 1.06 KiB** |
| load-time scratch in `hydra_build_aggregation()` | `int64_t[64]` × 2 | 1024 B, released on return |

`src/hydra_engine.c` contains no `malloc`/`calloc`/`realloc` in any path. This
is **[CODE]**, not a claim about behaviour.

So the *engine's own* contribution to Peak-PSS is ~1.1 KiB and does not grow with
the model. Every remaining term in §1.4 is about the model.

### 1.4 The four terms, and who owns each

Peak-PSS for a model-loading app decomposes into four terms with **different
owners** and **different remedies**. Conflating them is how a memory argument
goes wrong.

| Term | Owner | Reclaimable by the OS? |
|---|---|---|
| **1. Runtime buffers** (JNI staging, ART objects, the 1.06 KiB engine set) | the app | Only by the app, and it should not |
| **2. Working set** (the `A[i]/B[i]` aggregates, the KV/state vector, whatever v2 decides is resident) | the app | Only by the app, and it should not |
| **3. Mapped model pages** — file-backed, read-only, touched | the kernel page cache | **Yes.** They are Private Clean and the OS drops them under pressure |
| **4. Swap / ZRAM** | the system | No — it is a sign the device is already in trouble |

**Term 3 is the one this project keeps arguing about, and it is the one the app
does not own.** A read-only `mmap(PROT_READ, MAP_SHARED)` of a model file
produces pages that are: clean (never written), file-backed, and therefore
eligible for reclaim without swap and without writeback. **[SOURCE]** — Android's
memory documentation states that Private Clean memory "can be dropped by the OS
if memory is low".

### 1.5 The honest summary of §1

- **[CODE]** Engine-owned resident memory is ~1.06 KiB, independent of model size.
- **[CODE]** The v1 weight region is read **once**, at load, by
  `hydra_build_aggregation()`; `hydra_engine_step()` never touches it.
- **[NOT MEASURED]** Peak-PSS of this app on any physical Android device. No
  device has been available. Every timing and memory figure that exists for this
  project was taken on an x86-64 Linux container or a TCG-emulated x86_64
  Android guest, and is labelled as such wherever it appears.
- **[SOURCE]** Google's own published memory metric for apps is
  **"Memory usage (Anonymous RSS + Swap)"** — it explicitly counts *anonymous*
  RSS, and a file-backed read-only mapping is not anonymous. So a mapped model
  does not push the app toward that core-vital threshold, whereas a model
  **copied into the heap would.** This is the strongest concrete argument for
  keeping weights mapped and never read them into memory.

---

## 2. Load-strategy benchmark plan

### 2.1 The three strategies to compare

All three load the **same** file and then run the **same** token sequence. Only
the access strategy differs.

| # | Strategy | Mechanism | Hypothesis |
|---|---|---|---|
| **S1** | `mmap` + `MADV_SEQUENTIAL` | Map read-only, one sequential walk at load to build the aggregates | Fastest cold, best locality, page cache does the work |
| **S2** | `pread` + `posix_fadvise(POSIX_FADV_DONTNEED)` | Read chunks with `pread`, then advise the kernel to drop each chunk after use | Same I/O, lower peak page-cache residency — **if** the advice is obeyed |
| **S3** | chunked `pread` with a bounded ring buffer | Read chunks into a fixed ring, reuse it, never map the file | Bounded resident set by construction, independent of kernel behaviour |

**S1 is what the engine does today. [CODE]** (`src/hydra_engine.c`: one
`mmap(PROT_READ, MAP_SHARED)`, one `madvise(MADV_SEQUENTIAL)`, one
`hydra_build_aggregation()` walk.) S2 and S3 are **[PLANNED]** and do not exist.

### 2.2 The question the benchmark must answer

> Does explicitly advising the kernel to drop model pages reduce system memory
> pressure — or only reduce *this process's* RSS while the pages stay in the page
> cache and the device is no less pressed?

This is not a rhetorical question, and the kernel documentation says the honest
thing about it:

- `posix_fadvise(POSIX_FADV_DONTNEED)` "**attempts** to free cached pages" — the
  advice "is not binding; it merely constitutes an expectation", requests
  covering partial pages are **ignored**, and dirty pages are "not guaranteed"
  to be written back. **[SOURCE]** man7 `posix_fadvise(2)`.
- `madvise(MADV_DONTNEED)` on a **shared** mapping "**might not lead to immediate
  freeing** of the pages in the range. The kernel is free to delay freeing the
  pages until an appropriate moment. **The resident set size (RSS) of the
  calling process will be immediately reduced however.**" **[SOURCE]** man7
  `madvise(2)`.

That last sentence is the whole reason this is a benchmark and not a design
decision: **RSS can drop while system pressure does not.** And system pressure is
what actually gets a process killed — Android's `lmkd` decides from kernel
**pressure stall information (PSI)**, "the amount of time that tasks are delayed
as a result of memory shortages", which is the **default** detection mechanism
since Android 10 (`ro.lmk.use_psi` defaults to `true`). **[SOURCE]** AOSP `lmkd`
documentation.

> **No published evidence was found that `posix_fadvise` or `MADV_DONTNEED`
> prevents lmkd kills, or reduces lmkd kill rates, on Android.**
> **[NOT MEASURED]** — and deliberately left that way rather than filled in with
> a plausible story. The kernel semantics quoted above are suggestive, not
> dispositive. **The benchmark decides, not this document.** Until it has been
> run, any claim that S2 "prevents OOM kills" is unsupported and must not be
> written anywhere.

This project has already been burned by a memory optimisation that sounded right
and measured worse: an explicit `MADV_DONTNEED` after every step was added to
return memory after loading and made measured RSS **worse** than without it,
because the mapping already uses `MADV_SEQUENTIAL`, which drops pages behind the
read cursor automatically. It was reverted and the instrumentation kept. **[CODE]**
`-DHYDRA_DROP_CACHE` still exists behind that flag; see `errors.md`.

### 2.3 Metrics — what to record

Recorded per (strategy × device × cache state × model size):

| Metric | How | Why |
|---|---|---|
| **Peak-PSS** | `adb shell dumpsys meminfo -a <pkg>`, sampled at ≥10 Hz through load and the first 100 generated tokens; take the max | §1.2 |
| **PSS by term** | `adb shell showmap <pid>`, summed over the model's mapping | separates term 2 from term 3 of §1.4 |
| **PSI stall time** | `/proc/pressure/memory` (`some avg10/60/300`, `total`) sampled alongside | the signal `lmkd` itself uses |
| **lmkd events** | `adb logcat -b all -s lmkd`, plus Play Console vitals → *Low memory killers (LMKs)* | the outcome that matters |
| **Bytes read per token** | `getrusage` / `getrusage(RUSAGE_SELF).ru_inblock` if available, else counted from the I/O path | separates bandwidth-bound from compute-bound |
| **TTFT** (time to first token) | wall clock from `load()` returning to the first `onTokens` callback | user-visible |
| **tokens/s** | steady-state, excluding warmup; report median of ≥5 runs | comparable across runs |
| **Thermal state** | `adb shell dumpsys thermals` before and after | a throttled run is not comparable to a cool one |

**Protocol requirements**, each of which exists because its absence produced a
wrong conclusion before:

1. **Same device, same OS build, same governor, no other app in the
   foreground.** Cross-device comparison of tok/s is meaningless.
2. **Cold AND warm cache, separately reported, never averaged.** Rebooting the
   device (or `echo 3 > /proc/sys/vm/drop_caches` where permitted) before each
   cold run. A single number blending the two states describes neither.
3. **≥5 repetitions, median reported, spread reported.** A single run on a
   shared or thermally throttled host is a sample, not a specification.
4. **The engine's own numbers come from the binary, not from a hard-coded
   literal.** `tools/test_count_check.sh` exists because the hard-coded version
   was wrong for years.
5. **Report NOT MEASURED for every cell of the table you did not fill in.**

### 2.4 What would falsify what

Stated in advance so the result cannot be reinterpreted after the fact:

- **S2 wins** only if Peak-PSS **and** PSI stall time both fall, *and* the
  tok/s cost is acceptable. Lower RSS alone is explicitly **not** a win (§2.2).
- **S2 loses** if Peak-PSS is unchanged while TTFT rises — that is the signature
  of advice the kernel declined to act on while the app paid the `pread` cost.
- **S3 wins** if it is the only strategy whose resident set is independent of
  cache behaviour, and only if the extra copies do not cost more bandwidth than
  they save.
- **Any strategy "wins" on TTFT alone** → it is a cold-cache measurement wearing
  a memory costume, and must be re-run warm.

---

## 3. The math: ternary weights and activations

### 3.1 What BitNet b1.58 actually specifies

From the primary source (Ma et al., *The Era of 1-bit LLMs*, arXiv:2402.17764,
submitted 27 Feb 2024). **[SOURCE]**

**Weight quantization — absmean, then round and clip:**

```
W̃ = RoundClip( W / (γ + ε), −1, +1 )

RoundClip(x, a, b) = max(a, min(b, round(x)))

γ = (1 / n·m) · Σ_ij |W_ij|
```

**[SOURCE]** — equations (1), (2), (3) of the paper.

**Activation quantization — 8-bit, symmetric, per token:**

```
s_x = Q_b / max|x|,   Q_b = 127 for int8
x̂   = round(x / s_x),  clamped to [−Q_b, Q_b]
```

The paper's stated change from BitNet is that activations are "all scaled to
[−Q_b, Q_b] per token **to get rid of the zero-point quantization**", instead of
being scaled to `[0, Q_b]` before the non-linearities. **[SOURCE]**

**This repository already does the weight half of this, and calls it what it is.**
`tools/gguf_to_hydra.py` computes `gamma = 1 / mean(|W|)` over exactly the rows it
then ternarises and reports it per plane as `meanAbs`/`gamma`. **[CODE]** That is
the *same* absmean rule as the paper's equation (3), applied as **post-training
quantization**.

### 3.2 The caveat that decides whether any of it matters

> **The paper trains from scratch.** BitNet b1.58 "is trained from scratch, with
> 1.58-bit weights and 8-bit activations" — the ternary constraint is present
> during training, so the network *learns around* it. **[SOURCE]**
>
> **Post-hoc rounding is a different operation with different results.** This
> project converts an already-trained FP16/BF16 checkpoint by rounding it to
> ternary afterwards. Those are not the same model, and the paper's perplexity
> and accuracy numbers **do not transfer** to a post-hoc conversion. Anything
> this project writes about accuracy after conversion must come from its own
> measurement on its own converted model, never from the paper.

**Where the reported speedups come from, and why they do not transfer.** The
paper's numbers (3B: 2.71× faster, 3.55× less GPU memory; 70B: 4.1× faster) are
measured with **FasterTransformer on NVIDIA GPU** and a hand-written 2-bit
kernel from Ladder. **[SOURCE]** They are properties of that kernel on that
hardware. An ARM CPU with no GPU kernel is a different machine by a wide margin,
so:

> **No speed, memory or energy claim may appear anywhere in this project until a
> ternary kernel has beaten the pinned llama.cpp baseline on the same device,
> in the same run.** rules.md R32: measure before claiming; "blocked, not
> measured" is a valid result. A proxy measurement of a different system is not
> a partial result — it is a measurement that will be quoted later as if it were
> the right one.

The **baseline** is already pinned and enforced: llama.cpp at v0.5.0
(`7fe450e19305b828c199d602c23a8337aaa1f03b`), verified by
`tools/llama_pin_check.sh` with a negative control. Any future ternary kernel is
compared against *that* build, on the same device, in the same session.

### 3.3 What Hydra-2 could actually reuse

The honest reuse list is short and worth writing down so nobody re-derives it:

| From BitNet b1.58 | Reusable here? | Why |
|---|---|---|
| absmean weight rule `γ = 1/mean|W|` | **Already implemented** **[CODE]** | same equation; already in `tools/gguf_to_hydra.py` |
| integer-addition matmul for ternary weights | **Partly** — v1's `2·d` form is already additions plus one multiply by a small integer | v1's recurrence is simpler than a transformer matmul, not harder |
| 8-bit symmetric per-token activation scaling | Not implemented **[PLANNED]** | needs a real forward pass; v1 has no activation pipeline |
| Training-from-scratch recipe | **No** — out of scope; this project converts, it does not train | see §3.2 |
| The 1.58× speed/latency figures | **No** | different hardware, different kernel (§3.2) |

---

## 4. Sparsity: what actually makes a too-big model possible

### 4.1 The arithmetic that decides this

Dense weight streaming is **bandwidth-bound**, and the bound is arithmetic, not
an implementation detail:

```
tokens/s  ≤  B_eff / W_per_token
```

where `B_eff` is the effective bandwidth actually achieved on the device and
`W_per_token` is the number of weight bytes that must be read to produce one
token. **[ESTIMATE]** — the inequality is a throughput ceiling; the constants
must be measured per device (§2.3).

Two consequences, and the second is the important one:

1. **Ternary weights make `W_per_token` small.** At v1's 2 bits per weight, a
   forward pass touching *n* weights reads `n/4` bytes. That raises the ceiling;
   it does not remove it.
2. **For a dense model larger than RAM, the ceiling collapses to zero.** If
   `W_per_token > B_eff × (time budget)`, no amount of arithmetic cleverness
   produces a token — the data cannot arrive in time. Skipping a fraction of the
   weights is the only way to lower `W_per_token`, so **sparsity is what converts
   "too big" into "slow"**, which is a solvable problem.

That is the entire argument for the sparse-MoE design in `FORMAT-V2.md`.

### 4.2 The only structure that gets you there

**Sparse mixture-of-experts**, where each token routes to `top_k` experts
(`v2.top_k` is required, not defaulted) and only those experts' weights are
read. Two different consequences follow, and conflating them is the usual error:

| | Dense streaming | Sparse-MoE expert streaming (`FORMAT-V2.md`) |
|---|---|---|
| Bytes per token | **all** weights (minus ternary compression) | **only the selected experts** |
| Models larger than RAM | Not viable — the bound in §4.1 goes to zero | **Viable**, because the working set is the experts for one token, not the model |
| What it costs | Nothing extra | A routing decision per token, a routing table, and an expert-cache miss penalty |
| Where it lives | today, v1 **[CODE]** | `FORMAT-V2.md`, **[SPEC]** — nothing implemented |

> **Dense streaming stays bandwidth-bound and does not escape it.** Ternary
> compression raises the achievable token rate by making `W_per_token` smaller,
> and that is the honest extent of it. **Only expert streaming makes a
> larger-than-RAM model viable**, and that is the FORMAT-V2 path, and it is not
> built.

### 4.3 What v1 can and cannot express today **[CODE]**

The v1 container caps the weight region at **256 KiB** (`dim ≤ 64`,
`layers ≤ 4096`, one byte per `(layer, dim)` pair — `include/hydra_model.h`).
A parameter count far beyond a few million is not expressible in v1 at all.
Lifting that ceiling is v2's job, and the *resident* side of it is this
document's subject.

---

## 5. Open questions this document does not answer

Each is open because the answer requires a measurement that has not been made.

1. **[PLANNED]** Which of S1/S2/S3 (§2.1) minimises **Peak-PSS** on a real phone,
   and at what cost in TTFT and tok/s?
2. **[PLANNED]** Does any load strategy move the **PSI stall** numbers at all?
   (§2.2 — this is the question that decides whether lmkd behaviour changes.)
3. **[NOT MEASURED]** Peak-PSS of this app on any physical Android device.
4. **[NOT MEASURED]** Throughput of any ternary transformer kernel on any ARM
   Android CPU. No such kernel exists in this repository.
5. **[NOT MEASURED]** The `B_eff` and `W_per_token` constants in §4.1 for any
   device.
6. **[OPEN]** Whether the KV/state term (§1.4, term 2) or the mapped-pages term
   (term 3) dominates for a *real* v2 model on a *real* phone. The design in
   `FORMAT-V2.md` §6 projects a budget; that projection is an **[ESTIMATE]** and
   has never been checked against hardware.

**Until each of these is measured on a named device, it stays open.** Writing
down a plausible number would be worse than leaving the question visible —
`errors.md` records, more than once, that a number remembered from a plausible
derivation is how documentation ends up contradicting the code.

---

## 6. Sources

Primary sources, fetched and read while writing this document:

| Claim area | Source |
|---|---|
| Absmean ternary quantisation, `RoundClip`, 8-bit activations, training-from-scratch, GPU speed/latency figures | Ma et al., *The Era of 1-bit LLMs: All Large Language Models are in 1.58 Bits*, arXiv:2402.17764 — <https://arxiv.org/html/2402.17764v1> |
| PSS / Private Dirty / Private Clean / SwapPSS semantics; `dumpsys meminfo`; `showmap` | Android Developers, *Quick assessment tools* — <https://developer.android.com/topic/performance/memory/guide/tools-overview> |
| lmkd, PSI monitors as the default mechanism, `ro.lmk.use_psi` default `true`, kill thresholds | AOSP, *Low memory killer daemon* — <https://source.android.com/docs/core/perf/lmkd> |
| Published Play memory metric is *Anonymous RSS + Swap*; bad-behaviour thresholds by RAM tier; LMKs listed under "all other vitals" | Android Developers, *Android vitals* — <https://developer.android.com/google/play/vitals> |
| `POSIX_FADV_DONTNEED` is advisory, page-aligned, not guaranteed | man7 `posix_fadvise(2)` — <https://man7.org/linux/man-pages/man2/posix_fadvise.2.html> |
| `MADV_DONTNEED` on a shared mapping may not free pages immediately, but RSS drops | man7 `madvise(2)` — <https://man7.org/linux/man-pages/man2/madvise.2.html> |

In-repository code cited above is listed by file and symbol in
[`FORMAT-V2.md`](FORMAT-V2.md) § *Provenance rule*; the format itself is a
specification and nothing in it is implemented.