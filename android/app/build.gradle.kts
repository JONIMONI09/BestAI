plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.hydrastone"
    compileSdk = 34

    defaultConfig {
        applicationId = "dev.hydrastone"
        minSdk = 24
        targetSdk = 34
        // Derived from the git tag in the release workflow (v1.2.3 -> 10203),
        // so the APK is unambiguously tied to a version.
        versionCode = (System.getenv("HYDRA_VERSION_CODE") ?: "1").toInt()
        versionName = System.getenv("HYDRA_VERSION_NAME") ?: "1.0.0"

        ndkVersion = "26.3.11579264"

        ndk {
            // armeabi-v7a is the legacy 32-bit target from the original spec;
            // arm64-v8a is the modern default.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=none"
                cppFlags += ""
            }
        }

        lint {
            // GradleDependency only reports "a newer version is available".
            // The dependency is pinned deliberately (see build.gradle.kts
            // dependencies), so this warning is noise on every single build
            // and it would break the release gate, which requires a report
            // without findings.
            disable += "GradleDependency"
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // An unconfigured secret arrives as an EMPTY string, not as null, and
    // file("") throws "path may not be null or empty string" - that killed
    // the first real release run. takeIf { isNotBlank() } treats "unset"
    // and "empty" as the same thing.
    val keystorePath = System.getenv("HYDRA_KEYSTORE")?.takeIf { it.isNotBlank() }
    val keystoreUsable = keystorePath != null && file(keystorePath).exists()

    signingConfigs {
        // Release signing: prefer a real keystore from the CI secrets.
        // Without them we fall back to the debug key so the APK stays
        // installable - the release body then states explicitly that it is
        // debug-signed (no fake).
        create("release") {
            if (keystoreUsable) {
                storeFile = file(keystorePath!!)
                storePassword = System.getenv("HYDRA_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("HYDRA_KEY_ALIAS")
                keyPassword = System.getenv("HYDRA_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (keystoreUsable) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    // Only the Activity Result API for the SAF model picker. No AppCompat,
    // no Compose, no Material Components - the UI stays programmatic Views.
    // The -ktx artifact, not the plain one: lint's KtxExtensionAvailable
    // check flags the plain artifact as an informational finding, and the
    // release gate requires a report without findings.
    // 1.9.3 is the newest androidx.activity that still builds against
    // compileSdk 34; 1.13.0 requires compileSdk 36. Bumping the compile SDK
    // is a toolchain change, not a bug fix, so it is not smuggled in here.
    implementation("androidx.activity:activity-ktx:1.9.3")
}