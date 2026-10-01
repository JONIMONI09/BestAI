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
        versionCode = 1
        versionName = "1.0"

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

    buildTypes {
        release {
            isMinifyEnabled = false
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
