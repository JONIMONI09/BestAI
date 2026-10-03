plugins {
    id("com.android.application") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "2.0.0" apply false
    // Kotlin 2.0 ships the Compose compiler as its own Gradle plugin. The
    // version is the Kotlin version on purpose: the plugin must match the
    // compiler that compiles the Kotlin sources, so it is pinned to the same
    // 2.0.0 rather than floated (rules.md R15).
    id("org.jetbrains.kotlin.plugin.compose") version "2.0.0" apply false
}
