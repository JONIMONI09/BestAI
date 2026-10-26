# Android Skill: Building Hydra-Stone for Android with the NDK

A detailed, reproducible guide based on the actual integration we
performed (PR #6) — including every error we hit and how it was fixed.
If you need to port the engine to Android again (or debug the existing
port), start here.

## 1. What the integration actually required

| Component | Version used | Why needed |
|---|---|---|
| OpenJDK | 17 | Android Gradle Plugin 8.x requires JDK 17 |
| Android SDK cmdline-tools | commandlinetools-linux-11076708 | bootstrap the SDK without Android Studio |
| SDK packages | `platform-tools`, `platforms;android-34`, `build-tools;34.0.0` | manifest/res merge, dex, packaging |
| Android NDK | 26.3.11579264 (r26d) | compiles `src/hydra_engine.c` for ARM/x86 |
| CMake | 3.22.1 (SDK-managed) | AGP's `externalNativeBuild` drives it |
| Gradle | 8.7 + AGP 8.5.2 + Kotlin 2.0.0 | build orchestration |

Bootstrap (headless Linux, no Android Studio):

```bash
sudo apt-get install -y openjdk-17-jdk-headless
mkdir -p /opt/android-sdk && cd /opt/android-sdk
curl -sS -O https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
unzip commandlinetools-linux-11076708_latest.zip
mkdir -p cmdline-tools/latest
mv cmdline-tools/bin cmdline-tools/lib cmdline-tools/NOTICE.txt \
   cmdline-tools/source.properties cmdline-tools/latest/
yes | cmdline-tools/latest/bin/sdkmanager --licenses
cmdline-tools/latest/bin/sdkmanager \
    "platform-tools" "platforms;android-34" "build-tools;34.0.0" \
    "ndk;26.3.11579264" "cmake;3.22.1"
mkdir -p /opt/gradle && cd /opt/gradle
curl -sL -O https://services.gradle.org/distributions/gradle-8.7-bin.zip
unzip gradle-8.7-bin.zip
```

## 2. Project layout

The key design decision: **the Android build compiles the same
`src/hydra_engine.c` as the desktop CLI** — no engine fork. The JNI
layer lives in `android/app/src/main/cpp/` and reaches up into the repo
root for the shared sources:

```
android/
├── app/src/main/cpp/CMakeLists.txt   ← references ../../../../..(repo)/src/hydra_engine.c
├── app/src/main/cpp/hydra_jni.c      ← JNI bridge (load/step/unload + axiom)
├── app/src/main/java/dev/hydrastone/
│   ├── HydraBridge.kt                ← object + external fun + Callback interface
│   └── MainActivity.kt               ← minimal UI, token log
├── app/src/main/assets/starter.hydra    ← packed starter model (from tools/make_model.py)
├── app/build.gradle.kts              ← externalNativeBuild + ndkVersion pinned
└── build.gradle.kts / settings.gradle.kts / gradle.properties
```

### The CMake path trap (read this twice)

From `android/app/src/main/cpp/`, the repo root is **five** levels up,
not four:

```
cpp → main → src(app) → app → android → <repo root>
```

Our first two attempts used `../../src` and `../../../../..`+1 and CMake
failed with `No SOURCES given to target: hydra`. The working form:

```cmake
get_filename_component(HYDRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
add_library(hydra SHARED ${HYDRA_ROOT}/src/hydra_engine.c ...)
target_include_directories(hydra PRIVATE ${HYDRA_ROOT}/include)
```

`get_filename_component(... ABSOLUTE)` is essential — relative paths in
`add_library` break when the AGP invokes CMake with different working
directories.

### JNI bridge essentials

- `Java_dev_hydrastone_HydraBridge_runInference` — the symbol name must
  match the Kotlin package/class exactly or `UnsatisfiedLinkError`
  strikes at runtime, not compile time.
- Token streaming back to the UI is **batched**, not per token:
  `GetObjectClass` → `GetMethodID` (`"([IZ)V"` signature, i.e.
  `onTokens(int[] tokens, boolean done)`) → tokens are staged into a
  `jint[HYDRA_JNI_BATCH]` array (`hydra_batch.h`, currently **16**) and
  delivered with **one** `CallVoidMethod` per block, plus
  `DeleteLocalRef` for the `int[]` and for the class. Local refs leak fast
  in loops without it.
  The old per-token shape `onToken(int step, int token)` with the
  `"(II)V"` signature is **still present**, but only on the benchmark
  path: `HydraBridge.benchmark()` uses it so the cost of the calling
  convention can be *measured* rather than assumed
  (`android_crash_check.sh` and `jni-batch-test` both depend on the
  distinction). Anything new should use `onTokens`.
- `HYDRA_JNI_BATCH` is `16`, not a tunable: `tests/test_jni_batch.c`
  asserts the exact block sizes, so changing the constant without
  changing that test is how a batching regression ships.
- **Model must be a real file**: the engine's loader is `mmap`-based
  and assets live inside the APK zip. Copy the asset to
  `context.filesDir` on first launch, then mmap works untouched.
- Return a JSON summary string via `NewStringUTF` and log with
  `__android_log_print` (tag `HydraJNI`) — logcat is the primary
  verification surface on a headless CI emulator.

### Build config that matters

```kotlin
defaultConfig {
    ndkVersion = "26.3.11579264"   // must match the installed NDK
    ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
    externalNativeBuild { cmake { arguments += "-DANDROID_STL=none" } }
}
externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }
```

- `ANDROID_STL=none` is fine here: the engine is pure C99 + liblog.
- `x86_64` is in the ABI list so API-24 emulator images can run the
  library without ARM translation.
- The NDK defines `__ARM_NEON` on ARM ABIs automatically — the engine's
  NEON kernel activates with zero extra flags.

## 3. Every error we hit, and the fix

### Error 1: `No SOURCES given to target: hydra`

- **Symptom:** CMake configure fails during `gradle assembleDebug`.
- **Cause:** wrong relative path depth (`../../src`) — `cpp/../../src`
  resolves to `android/app/src/main/src`, which doesn't exist.
- **Fix:** `get_filename_component(HYDRA_ROOT …/../../../../.. ABSOLUTE)`
  (five `..`), then reference `${HYDRA_ROOT}/src/hydra_engine.c`.
- **Lesson:** count levels from the CMakeLists location, not from the
  repo root; verify with a bare CMake run before invoking Gradle (see
  §4).

### Error 2: implicit declaration of `snprintf` / `clock` / `CLOCKS_PER_SEC`

- **Symptom:** clang in the NDK fails with
  `-Wimplicit-function-declaration` errors for stdio/time symbols.
- **Cause:** host `gcc` tolerated missing includes via transitive
  headers; NDK clang is stricter.
- **Fix:** add `#include <stdio.h>` and `#include <time.h>` to
  `hydra_jni.c`.
- **Lesson:** NDK clang is a stricter lint gate than desktop gcc —
  treat a clean NDK build as part of the test suite.

### Error 3: AGP picked a nonexistent NDK (26.1 vs installed 26.3)

- **Symptom:** Gradle configures CMake against
  `/opt/android-sdk/ndk/26.1.10909125` — not installed → build fails.
- **Cause:** AGP has a default `ndkVersion` per release; SDK Manager
  had installed 26.3.
- **Fix:** pin `ndkVersion = "26.3.11579264"` in `defaultConfig` and
  install exactly that version via sdkmanager.
- **Lesson:** always pin `ndkVersion` to a version you actually
  installed; never rely on AGP defaults.

### Error 4: first emulator run — `Unknown AVD name`

- **Symptom:** `emulator -avd hydra_test` errors although avdmanager
  reported success.
- **Cause:** the AVD was created under `/root/.android/avd`, but the
  emulator looked at `$HOME/.android/avd` where `$HOME` differed.
- **Fix:** export `ANDROID_SDK_HOME=/root` and
  `ANDROID_AVD_HOME=/root/.android/avd` before launching.
- **Lesson:** on headless CI, always set `ANDROID_AVD_HOME` explicitly.

### Error 5: emulator missing `libX11.so.6`

- **Symptom:** the emulator binary aborts at startup.
- **Cause:** headless server without X11 runtime libs.
- **Fix:**
  `apt-get install -y libx11-6 libxext6 libxrandr2 libxrender1 libgl1 libpulse0 libnss3 libxcb1 libxkbcommon0 libasound2`
- **Lesson:** even `-no-window` needs the X11/GL shared objects linked
  into the emulator binary.

### Error 6: no `/dev/kvm`

- **Symptom:** nested virtualization unavailable in the container. With no
  acceleration flag at all the emulator 37.x refuses to start and exits with
  `ERROR | x86_64 emulation currently requires hardware acceleration!`.
- **Fix:** run the emulator with `-accel off -gpu swiftshader_indirect`.
  (`-accel off` is the valid form; `-no-accel` is not a recognised flag on
  current emulator builds.) An x86_64 image + software rendering boots in
  ~1–2 min on a modern container CPU. Avoid ARM system images here — they
  would emulate on QEMU TCG and take many minutes.
- **Lesson:** KVM is a nice-to-have; x86_64 + `-accel off` is a working
  fallback for CI containers. Do **not** add `-wipe-data` on a slow host: it
  forces a from-scratch guest init that can trip the emulator's own watchdog.

### Error 7: the emulator dies at boot and blames the hypervisor

- **Symptom:** the guest aborts during startup with
  `ERROR | detected a hanging thread 'QEMU2 main loop'. No response for 15847 ms`,
  and `adb devices` lists nothing. The log's *last* lines matter; the
  acceleration warning is printed early and looks like the cause.
- **Cause:** not the emulator. Two Gradle daemons were holding ~2.4 GB of a
  3.9 GB container. QEMU's threads could not be scheduled inside its own
  watchdog window, so the emulator aborted *itself*. No emulator flag fixes a
  host with no free memory.
- **Fix:** `gradle --stop` before starting the emulator, and give the AVD
  enough RAM (`hw.ramSize`, or `-memory 1536`). It then boots in ~90 s with
  zero hang errors.
- **Lesson:** before blaming a sandboxed hypervisor, check host memory.
  `free -m` during the failure showed 3.5 GB *available* once the daemons were
  stopped, versus ~1.2 GB before — that single number identified the cause.

### Error 8: dim/vocab/layers printed as 0 in the JSON summary

- **Symptom:** on-device logcat showed
  `{"ok":true,"steps":32,"dim":0,"vocab":0,"layers":0,…}`.
- **Cause:** the JNI code read `engine.header.*` **after**
  `hydra_engine_unload(&engine)` — which `memset`s the struct.
- **Fix:** copy the header fields into locals before unload, then
  build the JSON from those.
- **Lesson:** classic use-after-free-adjacent bug — visible only in
  on-device logs, invisible to host unit tests because the host test
  asserted different fields. Runtime verification on the real target
  platform catches what unit tests cannot.

### Runtime verification loop (how we confirmed it works)

```bash
export PATH=/opt/android-sdk/platform-tools:$PATH
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n dev.hydrastone/.MainActivity
adb shell input tap 160 274        # coordinates from uiautomator dump
adb shell "logcat -d -s HydraJNI:*"
# → {"ok":true,"steps":32,"elapsed_us":1482,"dim":64,"vocab":512,
#    "layers":4,"axiom_allowed":true}
adb shell uiautomator dump && adb shell cat /data/local/tmp/window_dump.xml
# → shows the streamed tokens in the UI TextView
```

The token sequence on-device was identical to the host CLI run of the
same model — the engine is deterministic across host (x86_64) and
Android (same ABI, scalar path).

## 4. Debug workflow that saved the most time

Before touching Gradle, validate the native build directly with the
SDK's CMake + toolchain — errors are far more readable than through
AGP's wrapper:

```bash
SDK=/opt/android-sdk
$SDK/cmake/3.22.1/bin/cmake -Handroid/app/src/main/cpp \
  -DCMAKE_SYSTEM_NAME=Android -DANDROID_ABI=arm64-v8a \
  -DANDROID_NDK=$SDK/ndk/26.3.11579264 \
  -DCMAKE_TOOLCHAIN_FILE=$SDK/ndk/26.3.11579264/build/cmake/android.toolchain.cmake \
  -DCMAKE_MAKE_PROGRAM=$SDK/cmake/3.22.1/bin/ninja \
  -B/tmp/cmake_test -GNinja
$SDK/cmake/3.22.1/bin/cmake --build /tmp/cmake_test
$SDK/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf -h /tmp/cmake_test/libhydra.so
```

Similarly, verify APK contents without a device:

```bash
unzip -l app/build/outputs/apk/debug/app-debug.apk | grep -E "libhydra|starter.hydra|classes.dex"
$SDK/ndk/*/…/llvm-nm -D libhydra.so | grep runInference
$SDK/build-tools/34.0.0/apksigner verify --print-certs app-debug.apk
```

## 5. What is still open

- Only debug signing (apksigner with the debug keystore). Release
  signing/Play setup is a follow-up.
- APK size is unoptimized (3 ABIs × debug). App bundles (.aab) or
  per-ABI splits can shrink it if distribution matters.
