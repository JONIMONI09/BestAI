package androidx.compose.ui.text.font
class FontFamily private constructor() {
    companion object {
        val Monospace: FontFamily = FontFamily()
        val SansSerif: FontFamily = FontFamily()
    }
}
enum class FontWeight {
    Thin, Light, Normal, Medium, SemiBold, Bold, Black
}
