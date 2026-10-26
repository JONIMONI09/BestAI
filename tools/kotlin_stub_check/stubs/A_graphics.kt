package android.graphics
class Typeface private constructor() {
    companion object {
        @JvmField val MONOSPACE: Typeface = Typeface()
        @JvmField val SANS_SERIF: Typeface = Typeface()
        @JvmStatic fun create(family: String, style: Int): Typeface = Typeface()
        const val BOLD = 1
        const val NORMAL = 0
    }
}
