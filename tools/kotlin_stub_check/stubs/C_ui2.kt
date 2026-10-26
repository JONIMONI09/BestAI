package androidx.compose.ui

interface Modifier {
    companion object : Modifier
    fun then(other: Modifier): Modifier = other
}

interface Alignment {
    companion object {
        val Center: Alignment = object : Alignment {}
        val CenterVertically: Alignment = object : Alignment {}
        val CenterHorizontally: Alignment = object : Alignment {}
        val Top: Alignment = object : Alignment {}
        val Bottom: Alignment = object : Alignment {}
        val Start: Alignment = object : Alignment {}
        val End: Alignment = object : Alignment {}
        val TopStart: Alignment = object : Alignment {}
        val TopCenter: Alignment = object : Alignment {}
        val BottomCenter: Alignment = object : Alignment {}
    }
}

object UnitHolder
