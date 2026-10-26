---
name: android-ndk-build
description: "Build and verify the Hydra-Stone Android APK via NDK/JNI: toolchain bootstrap, gradle build, no-KVM emulator verification, and the 7 known pitfalls with fixes. Verified end-to-end twice; includes the exact expected outputs."
---

# Android NDK Build — Hydra-Stone

Build and verify the Android APK that wraps the Hydra-Stone C engine
via JNI. Full experience-based guide: `docs/ANDROID_SKILL.md`.

## When to use

- "Build the Android app / APK"
- "Test the engine on Android"
- Debugging `libhydra.so` or JNI errors

## Toolchain (install once)

```bash
sudo apt-get install -y openjdk-17-jdk-headless
mkdir -p /opt/android-sdk && cd /opt/android-sdk
curl -sS -O https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
unzip commandlinetools-linux-11076708_latest.zip
mkdir -p cmdline-tools/latest
mv cmdline-tools/bin cmdline-tools/lib cmdline-tools/NOTICE.txt \
   cmdline-tools/source.properties cmdline-tools/latest/
yes | cmdline-tools/latest/bin/sdkmanager --licenses
cmdline-tools/latest/bin/sdkmanager "platform-tools" "platforms;android-34" \
    "build-tools;34.0.0" "ndk;26.3.11579264" "cmake;3.22.1"
mkdir -p /opt/gradle && cd /opt/gradle
curl -sL -O https://services.gradle.org/distributions/gradle-8.7-bin.zip
unzip gradle-8.7-bin.zip
```

## Build

```bash
export ANDROID_HOME=/opt/android-sdk
export JAVA_HOME=$(dirname $(dirname $(readlink -f $(which java))))
cd android && /opt/gradle/gradle-8.7/bin/gradle assembleDebug --no-daemon
# APK: android/app/build/outputs/apk/debug/app-debug.apk  (~0.9 MB)
```

Speed tip: drop `--no-daemon` for repeated local builds (17s vs 27s+
cold). Keep `--no-daemon` in CI (clean, no leftover JVM).

### Verify the APK before installing

```bash
APK=app/build/outputs/apk/debug/app-debug.apk
unzip -l $APK | grep -E "libhydra|starter.hydra|classes.dex"
# expect: lib/{arm64-v8a,armeabi-v7a,x86_64}/libhydra.so + assets/starter.hydra
/opt/android-sdk/build-tools/34.0.0/apksigner verify $APK && echo "signature: OK"
unzip -o $APK lib/arm64-v8a/libhydra.so -d /tmp/apkchk >/dev/null
/opt/android-sdk/ndk/26.3.11579264/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm -D \
  /tmp/apkchk/lib/arm64-v8a/libhydra.so | grep runInference
# expect: Java_dev_hydrastone_HydraBridge_runInference
```

## Verify on emulator (works without KVM)

One-time setup (AVD + X11 libs for the emulator binary):

```bash
sudo apt-get install -y libx11-6 libxext6 libxrandr2 libxrender1 libgl1 \
    libpulse0 libnss3 libxcb1 libxkbcommon0 libasound2
echo no | $ANDROID_HOME/cmdline-tools/latest/bin/avdmanager create avd \
    -n hydra_test -k "system-images;android-24;default;x86_64" --force
```

Start + verify (cold boot takes 1-2 min without KVM; poll, do not
assume a fixed sleep — a background start can outlive the terminal):

```bash
export PATH=/opt/android-sdk/platform-tools:$PATH
export ANDROID_AVD_HOME=/root/.android/avd
nohup $ANDROID_HOME/emulator/emulator -avd hydra_test -no-window -no-audio \
    -no-boot-anim -gpu swiftshader_indirect -no-accel -memory 2048 \
    > /tmp/emulator.log 2>&1 &
adb devices                       # wait until "emulator-5554  device"
adb shell 'while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 2; done'
# POSIX [] on purpose: Android uses mksh; bash [[ ]] is not guaranteed
```

Install + run:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk   # from android/
adb shell am start -n dev.hydrastone/.MainActivity
adb shell input tap 160 274        # "Run inference" (see note below)
sleep 5
adb shell "logcat -d -s HydraJNI:*"
# expect: {"ok":true,"steps":32,"elapsed_us":~1000-15000,"dim":64,
#          "vocab":512,"layers":4,"axiom_allowed":true}
adb shell uiautomator dump /data/local/tmp/ui.xml
adb shell cat /data/local/tmp/ui.xml | tr '<' '\n' | grep "Hydra"
# expect: streamed tokens token[0..7] identical to the host CLI run
```

Tap note: `160 274` targets a 320x640 screen. For any other screen,
find the button via uiautomator dump and tap its bounds center:

```bash
adb shell uiautomator dump /data/local/tmp/ui.xml
adb shell cat /data/local/tmp/ui.xml | grep -o 'RUN INFERENCE[^/]*bounds="\[[0-9]*,[0-9]*\]\[[0-9]*,[0-9]*\]"'
# tap the center of the reported bounds
```

When done:

```bash
adb emu kill
```

## Known pitfalls (all hit and fixed — details in docs/ANDROID_SKILL.md)

1. **CMake source path**: repo root is 5 `..` from `app/src/main/cpp`, use
   `get_filename_component(HYDRA_ROOT ... ABSOLUTE)`.
2. **NDK clang is stricter** than desktop gcc: include `<stdio.h>`/`<time.h>`
   explicitly.
3. **Pin `ndkVersion`** in `app/build.gradle.kts` to the installed NDK.
4. **Set `ANDROID_AVD_HOME`** or the emulator can't find the AVD.
5. **Emulator needs X11 libs** even with `-no-window`.
6. **No `/dev/kvm`**: use `-no-accel -gpu swiftshader_indirect` + x86_64 image.
7. **Model must be a real file**: copy the APK asset to `context.filesDir`
   before calling the engine (its loader uses mmap).
8. **Boot wait must be portable + bounded**: use POSIX `[ ]` with
   `getprop sys.boot_completed`, never bash `[[ ]]` via adb shell.

## Quick native-only debug (skip Gradle)

```bash
SDK=/opt/android-sdk
$SDK/cmake/3.22.1/bin/cmake -Handroid/app/src/main/cpp \
  -DCMAKE_SYSTEM_NAME=Android -DANDROID_ABI=arm64-v8a \
  -DANDROID_NDK=$SDK/ndk/26.3.11579264 \
  -DCMAKE_TOOLCHAIN_FILE=$SDK/ndk/26.3.11579264/build/cmake/android.toolchain.cmake \
  -DCMAKE_MAKE_PROGRAM=$SDK/cmake/3.22.1/bin/ninja -B/tmp/cmake_test -GNinja
$SDK/cmake/3.22.1/bin/cmake --build /tmp/cmake_test
```

## Expected end-to-end result (verified 2x on API-24 x86_64)

- logcat `HydraJNI`: `{"ok":true,"steps":32,...,"dim":64,"vocab":512,"layers":4,"axiom_allowed":true}`
- UI token stream identical to host CLI (`./hydra-run models/starter.hydra 42 8 --json`):
  `211 388 401 330 111 176 113 170` — the engine is deterministic
  across host and Android on the same code path.
