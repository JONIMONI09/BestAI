package android.os
class Bundle {
    fun getBoolean(key: String, defaultValue: Boolean): Boolean = defaultValue
    fun putBoolean(key: String, value: Boolean) {}
}
object Build {
    @JvmField val SDK_INT: Int = 34
    @JvmField val DEVICE: String = "stub"
    @JvmField val PRODUCT: String = "stub"
    @JvmField val MANUFACTURER: String = "stub"
    @JvmField val MODEL: String = "stub"
    @JvmField val SUPPORTED_ABIS: Array<String> = arrayOf("arm64-v8a")
    object VERSION {
        const val RELEASE: String = "14"
        const val SDK_INT: Int = 34
    }
    object VERSION_CODES { const val R = 30; const val S = 31; const val T = 33 }
}
object SystemClock {
    @JvmStatic fun elapsedRealtime(): Long = 0L
}
class IBinder
