# GPU feasibility benchmark (Phase 2)

**Status: NOT MEASURED.** No Snapdragon 8 Elite tablet and no emulator were
available in the environment where this code was written, so this directory
contains a benchmark that has been *compiled for arm64* but **never run**.
Nothing in `docs/gpu-feasibility.md` is based on output from this program,
because there is no output yet. (Rule R32 in `rules.md`.)

## What it does

It runs the *same* ternary accumulate twice and prints both numbers as
JSON:

1. **CPU reference** — the exact arithmetic of `hydra_engine_step()` after
   layer aggregation (`acc[i] = A[i]*token + B[i]*state[i]`).
2. **GPU** — the same formula in a GLES 3.1 compute shader
   (`ternary.comp`), with `A`, `B` and `state` kept in SSBOs and the
   accumulator read back with `glGetBufferSubData`.

The decisive axis is **K, the number of steps per submit**. With one
dispatch per token the dispatch round-trip dominates. The sweep
`--k 1,16,64,256` shows where that stops being true.

The program refuses to report a GPU number that disagrees with the CPU
checksum: `checksum_matches_cpu` must be `true`, otherwise the two paths
are not comparable and the timing is meaningless.

## Build

The shader needs `glslangValidator`; there was none on the build host, so
the `.spv` is deliberately **not** committed (an unverified binary blob in
a repository is worse than a missing one):

```bash
glslangValidator -V --target-env opengl ternary.comp -o ternary.comp.spv

NDK=~/Android/Sdk/ndk/26.3.11579264
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
    -O2 gpu_bench.c -lEGL -lGLESv2 -o gpu_bench
adb push gpu_bench ternary.comp.spv /data/local/tmp/
```

## Run

```bash
adb shell "cd /data/local/tmp && ./gpu_bench --dim 64 --layers 4 --k 1,16,64,256 --steps 1000"
adb shell "cd /data/local/tmp && ./gpu_bench --dim 1024 --layers 64 --k 1,16,64,256 --steps 1000"
```

Each line of output is one JSON object, so it pipes straight into a
plotting script:

```json
{"device":"cpu","dim":64,"layers":4,"steps":1000,"ns_per_step":12.4,"measured":true}
{"device":"gpu","dim":64,"layers":4,"k_per_submit":16,"steps":1000,"ns_per_step":31.8,"ns_per_submit":509.0,"measured":true,"checksum_matches_cpu":true}
```

## Deliberate limitations

* **`--k` currently loops one dispatch per step.** The uniform block is
  uploaded once per dispatch, so the measured `k_per_submit` column is the
  cost of K sequential dispatches, not of a shader that internally loops K
  times. A real K-steps-per-submit kernel would move the loop *inside* the
  shader; that variant is not written yet. Treat the K column as an upper
  bound until it is.
* **No timeline semaphores, no persistent mapping.** Those only matter
  once the crossover is actually in sight.
* **Vulkan is not implemented.** GLES 3.1 compute was chosen because it is
  roughly a third of the code. Adreno 830 supports both.

## What the numbers must answer

Whether there exists a `(dim, layers, K)` where the GPU beats the optimised
CPU path. Only then does a `.hydra` v2 with interacting dimensions become
worth designing. Until then the answer is "stop", and that is a valid
result.