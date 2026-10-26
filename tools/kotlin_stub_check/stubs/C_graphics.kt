package androidx.compose.ui.graphics
class Color(val value: ULong) {
    companion object {
        val White: Color = Color(0xFFFFFFFFuL)
        val Black: Color = Color(0xFF000000uL)
        val Transparent: Color = Color(0uL)
    }
}
fun Color(value: Long): Color = Color(value.toULong())
