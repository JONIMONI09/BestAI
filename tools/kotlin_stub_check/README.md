# kotlin_stub_check

Local type-check of the Android app's Kotlin sources **without the Android
SDK**, using a faithful API stub classpath. This is the local equivalent of
the Kotlin compile step of the CI "Android APK" job, and it is what should
have caught `HydraApp.kt:1011` (`ClosedFloatingPointRange` has no `.last` /
`.first` - the round-5 SettingSlider) and the missing
`androidx.compose.foundation.layout.weight` import before CI.

## Why

The sandbox has no Android SDK and the network allowlist blocks the Maven
repository, so a real `./gradlew compileDebugKotlin` is not possible here.
The Kotlin compiler itself IS obtainable (npm registry):

```sh
mkdir -p /tmp/ktnpm && cd /tmp/ktnpm
npm install kotlin-compiler@2.4.21 javajre-linux-64@17.0.8
```

## Run

```sh
bash tools/kotlin_stub_check/kotlin_stub_check.sh
```

Exit 0 + "kotlin stub check: OK (0 errors)" = all Kotlin files type-check.

## How it works

* `stubs/` contains hand-maintained declarations of the exact Android /
  androidx / Compose / org.json API surface the app uses (same packages and
  signatures as the real libraries; the R class is generated from
  `android/app/src/main/res/values/strings.xml`).
* `kotlinc` first compiles the stubs to `stubs.jar`, then compiles
  `android/app/src/main/java/dev/hydrastone/**/*.kt` against it.

## Maintaining the stubs

* When the app starts using a new Android/compose API, add it to the matching
  `stubs/*.kt` file (real signatures - verify against
  developer.android.com / composables.com, never guess).
* When `res/values/strings.xml` changes, regenerate `stubs/R_gen.kt`
  (the generator is the one used on 2026-10-26: parse the XML, emit
  `object R { object string { ... } ... }` for every referenced namespace).
* A new file per package (Kotlin rule: one package per file).
