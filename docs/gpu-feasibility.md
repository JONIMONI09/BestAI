# GPU feasibility for Hydra-Stone on Android

**Headline:** the GPU question is **not answered yet**, because the
measurement that would answer it could not be taken. No Snapdragon 8 Elite
tablet and no emulator were reachable from the environment this document was
written in (`adb` is not installed, no device is attached). Everything
below separates *measured here* from *not measured* — nothing is estimated
and presented as a result (rule R32 in `rules.md`).

## 1. What was measured, and where

| Phase | Status | Where |
|---|---|---|
| Phase 0 — CLI benchmark, CPU paths | **measured** | this machine, x86-64 Linux container |
| Phase 1.1 — layer aggregation | **measured**, with equivalence tests | this machine + ARM under qemu |
| Phase 1.2 — token-only fast path | **measured**, with equivalence tests | this machine |
| Phase 1.3 — JNI batching | implemented and merged earlier | **not measured on a device** |
| Phase 0 — JNI round-trip costs | **blocked** (no device) | — |
| Phase 2 — GPU crossover table | **blocked** (no device, no shader compiler) | — |
| Phase 3 — v2 design | **not started, and deliberately so** | see §5 |

### Reproduce the CPU numbers

```bash
make -s all
python3 tools/make_dummy_model.py models/demo.hydra
./hydra-run models/demo.hydra 0 --bench 50000
```

## 2. Measured CPU results

Model: `dim=64`, `layers=4`, `vocab=512` (the shipped demo model),
50 000 steps, three consecutive runs on the development container
(x86-64, no NEON path):

| Run | full path | token-only path | speedup |
|---|---|---|---|
| 1 | 132.56 ns/token | 18.42 ns/token | 7.20x |
| 2 | 149.35 ns/token | 18.27 ns/token | 8.17x |
| 3 | 135.44 ns/token | 19.22 ns/token | 7.05x |

`tokens_equal: true` in all three: the fast path is not a different
model, it is the same token stream computed with 1/64th of the arithmetic.

A deliberately larger model (`dim=64`, `layers=256`) gives
132.32 ns/token vs 18.03 ns/token — **7.34x**. That is the important
number for the GPU discussion: 64 extra layers changed nothing, because
layer aggregation moved that work out of the per-token path entirely.

**These are container numbers, not Snapdragon numbers.** A Snapdragon 8
Erlite will differ in absolute terms. The *ratio* between the two paths is
the transferable result; the absolute nanoseconds are not.

## 3. Why this matters for the GPU question

The prior analysis (nine independent agents) concluded that a GPU dispatch
round-trip of 10–100 µs is 10–100x slower than the CPU work at the current
model size. What the measurements above show is that the CPU side got
**cheaper before the GPU question was even asked**:

* layer aggregation removed the `layers` factor from the per-token path
  (4 layers → 1 multiply-add per dimension);
* the token-only path removed the `dim` factor for pure inference
  (64 → 1).

So the CPU baseline the GPU would have to beat is now around **18 ns/token**
in this container — not "under 1 µs/token" as the earlier analysis
assumed for a pre-optimisation engine. A GPU round-trip cannot win against
18 ns/token at any plausible dispatch cost. **That is an argument against
building the GPU version at the current model size, and it is now backed by
a measurement instead of an estimate.**

The measurement that would settle it completely — the real
dispatch/read-back latency on the Adreno 830 — is still missing.

## 4. Phase 2 status: built, not run

`tools/gpu_bench/` contains a standalone GLES 3.1 compute benchmark:

* the CPU reference uses the identical arithmetic to the engine,
* the GPU path keeps `A`, `B` and `state` in SSBOs and reads the
  accumulator back with `glGetBufferSubData`,
* it sweeps `dim x layers x K` (steps per submit),
* it refuses to print a GPU timing whose checksum disagrees with the CPU
  one.

It **compiles for arm64** with the NDK 26.3.11579264 toolchain
(`aarch64-linux-android24-clang -O2 -c tools/gpu_bench/gpu_bench.c` exits 0)
and has **never been executed**. The GLSL shader
(`tools/gpu_bench/ternary.comp`) still has to be compiled to SPIR-V with
`glslangValidator`, which is not installed here; shipping an unverified
binary blob would be worse than shipping the source.

To get the table (5 minutes on the tablet):

```bash
glslangValidator -V --target-env opengl tools/gpu_bench/ternary.comp -o ternary.comp.spv
adb push gpu_bench ternary.comp.spv /data/local/tmp/
adb shell "cd /data/local/tmp && ./gpu_bench --dim 64   --layers 4  --k 1,16,64,256 --steps 1000"
adb shell "cd /data/local/tmp && ./gpu_bench --dim 1024 --layers 64 --k 1,16,64,256 --steps 1000"
```

Expected shape of the output (the *shape*, not the values — see §3 for why
the current CPU numbers already make a win unlikely):

```
{"device":"cpu","dim":64,"layers":4,"steps":1000,"ns_per_step":...,"measured":true}
{"device":"gpu","dim":64,"layers":4,"k_per_submit":16,"steps":1000,"ns_per_step":...,"checksum_matches_cpu":true}
```

Known limitation, stated in `tools/gpu_bench/README.md`: the current
implementation dispatches once per step, so the `k_per_submit` column is the
cost of K *sequential dispatches*, not of a shader that loops K times
internally. It is an upper bound until that variant is written.

## 5. Recommendation: do not build v2 yet

**Recommendation: stop before Phase 3.** A `.hydra` v2 with interacting
dimensions, dims up to 4096 and a Vulkan backend should only be designed
when the Phase 2 table shows a crossover. At the current model size the
measurement in §2 already puts the CPU baseline far below a plausible GPU
round-trip, so the precondition for v2 is not met.

What *is* worth doing, in order:

1. Run `tools/gpu_bench` on the tablet and paste the table here. It costs
   minutes and it converts the remaining argument from analysis to data.
2. Measure the JNI round-trip on the device (`HydraBridge.benchmark()`),
   which is the number that decides whether batching mattered.
3. Only if the table shows a crossover at a model size the project would
   actually ship, design v2.

## 6. Corrections to the task brief

Two items in the brief were already done in `main` before this work
started, and are recorded here so nobody re-does them:

* **JNI batching** — `HydraBridge.Callback` was already changed to
  `onTokens(IntArray, Boolean)` with 16-token blocks, in PR #26.
* **SAF model import** — already implemented in PR #26 (`OpenDocument`,
  streamed copy into `filesDir`, header validation, atomic rename).
* **`labs()` sign loss** — already fixed; `hydra_engine_step()` uses a
  symmetric signed modulo, covered by a regression test.
* **The release workflow** — `release.yml` does build and verify the APK
  and attaches it to the release; verified again while writing this.