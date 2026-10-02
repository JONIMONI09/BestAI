## Read this first

**The Phase 0 tablet measurement is BLOCKED.** No Snapdragon 8 Elite tablet, no emulator and no `adb` in this environment. Nothing in this PR claims a device number. `docs/gpu-feasibility.md` separates *measured here* from *not measured*.

Three items in the brief were **already merged** before this work (PR #24/#26) and were **not** redone: JNI batching, SAF import, the `labs()` sign fix, and the APK-in-release workflow.

## Measured here (x86-64 container, not a Snapdragon)

```
make -s all && python3 tools/make_dummy_model.py models/demo.hydra
./hydra-run models/demo.hydra 0 --bench 50000
```

| Run | full | token-only | speedup |
|---|---|---|---|
| 1 | 132.56 ns/token | 18.42 ns/token | 7.20x |
| 2 | 149.35 ns/token | 18.27 ns/token | 8.17x |
| 3 | 135.44 ns/token | 19.22 ns/token | 7.05x |

`dim=64, layers=256` gives 132.32 vs 18.03 ns/token (**7.34x**) — the layer count no longer matters, because it left the per-token path.

`tokens_equal: true` in every run: the fast path is the *same model*, not a cheaper approximation.

## Why this is bad news for the GPU, on measured grounds

The prior analysis assumed a CPU baseline of "<1 us/token". After aggregation **and** the token-only path that baseline is **~18 ns/token** here. A GPU dispatch round-trip cannot beat that. The missing piece is the real Adreno dispatch latency — which needs the tablet.

## Phase 2: built, compiled, never run

`tools/gpu_bench/` compiles for arm64 (NDK, exit 0) and has **never been executed**. The GLSL shader still needs `glslangValidator`, which is not installed here; the `.spv` is deliberately **not** committed rather than committed unverified. To get the table:

```bash
glslangValidator -V --target-env opengl tools/gpu_bench/ternary.comp -o ternary.comp.spv
adb push gpu_bench ternary.comp.spv /data/local/tmp/
adb shell 'cd /data/local/tmp && ./gpu_bench --dim 64 --layers 4 --k 1,16,64,256 --steps 1000'
```

## A bug this work almost shipped

Layer aggregation left `hydra_neon.h` **dead**. The ARM path would have silently fallen back to scalar while `test_neon_matches_scalar` still passed — both sides of the comparison being the same scalar loop. The kernel was rewritten to vectorise the aggregated multiply-add, and the equivalence test now compares against a reference implementation **inside the test file**, not the engine's own scalar branch.

## Everything else

* **Crash handler in all three apps.** Console (`window.onerror`, unhandled rejections, failed requests) → dialog with **Copy details**, clipboard API with a textarea fallback for plain-HTTP LAN use. Server (`uncaughtException`, `unhandledRejection`, failed requests) → bounded ring buffer at `GET /api/errors`, polled by the UI. Android → default `UncaughtExceptionHandler` writes `last-crash.txt` and starts `CrashActivity` (own process) with **Copy report**.
* **Permissions.** SAF needs none, so `ci.yml` now fails if any dangerous permission appears in the APK.
* **UI.** `:focus-visible` rings, `prefers-reduced-motion`, an empty state instead of a blank canvas, 44px touch targets.
* **rules.md R31/R32** — everything through a PR in English; measure before claiming.

## Verification (all executed here)

```
make test                            61/0
aarch64 + qemu (NEON)                67/0
gcc/clang -fsyntax-only -Werror      ok
cppcheck --error-exitcode=2          ok
ASan+UBSan                           61/0
npx eslint                           ok
node --test tools/server_test.js     18/18
node tools/console_train_test.js     ok
tools/jni_signature_check.sh         ok (2 native methods)
gradle assembleDebug/Release lint    No issues found
gpu_bench arm64 NDK compile          ok
```

**Negative controls** (each one must fail):

| Test | Injected regression | Result |
|---|---|---|
| Layer aggregation | `B[0]` shifted by one | 1 failure |
| Token-only path | `acc0` shifted by one | 1 failure |

## Not verified

* No device, no emulator → no JNI, GPU or SAF numbers.
* No browser → the console changes were checked headlessly, not visually.
* `gpu_bench` has never run; its `--k` column is currently K sequential dispatches, not K in-shader steps (documented in `tools/gpu_bench/README.md`).

## Recommendation

**Do not start Phase 3 / a v2 format yet.** The precondition — a measured crossover — is not met, and the CPU side got ~7x faster before the question was asked. Run `gpu_bench` on the tablet, paste the table into `docs/gpu-feasibility.md`, and decide from data.

Merge is the user's call (R14).