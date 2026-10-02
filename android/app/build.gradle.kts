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
        // Im Release-Workflow aus dem Git-Tag ableiten (v1.2.3 -> 10203),
        // damit das APK eindeutig einer Version zugeordnet ist.
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
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    signingConfigs {
        // Release-Signierung: bevorzugt ein echtes Keystore aus den
        // CI-Secrets. Ohne diese Secrets wird auf den Debug-Schluessel
        // zurueckgefallen, damit das APK trotzdem installierbar bleibt —
        // es wird dann im Release-Body ausdruecklich als Debug-signiert
        // gekennzeichnet (KEIN Fake).
        create("release") {
            val storePath = System.getenv("HYDRA_KEYSTORE")
            if (storePath != null && file(storePath).exists()) {
                storeFile = file(storePath)
                storePassword = System.getenv("HYDRA_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("HYDRA_KEY_ALIAS")
                keyPassword = System.getenv("HYDRA_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (System.getenv("HYDRA_KEYSTORE") != null &&
                file(System.getenv("HYDRA_KEYSTORE")!!).exists()
            ) {
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
}
