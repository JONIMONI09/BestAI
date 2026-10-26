package androidx.compose.ui.unit
class Dp(val value: Float) {
    operator fun plus(other: Dp): Dp = Dp(value + other.value)
    operator fun times(other: Float): Dp = Dp(value * other)
    companion object {
        val Unspecified: Dp = Dp(Float.NEGATIVE_INFINITY)
    }
}
val Int.dp: Dp get() = Dp(toFloat())
val Float.dp: Dp get() = Dp(this)
