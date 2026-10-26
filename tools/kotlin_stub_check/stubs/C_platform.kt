package androidx.compose.ui.platform
class ProvidableCompositionLocal<T>(val defaultValue: T) {
    val current: T get() = defaultValue
}
val LocalContext: ProvidableCompositionLocal<android.content.Context> = ProvidableCompositionLocal(android.content.Context())
