package androidx.compose.foundation.layout
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.Dp

interface Arrangement {
    companion object {
        val Start: Arrangement = object : Arrangement {}
        val Center: Arrangement = object : Arrangement {}
        val End: Arrangement = object : Arrangement {}
        @JvmStatic
        fun spacedBy(space: Dp): Arrangement = object : Arrangement {}
    }
    interface Horizontal : Arrangement
    interface Vertical : Arrangement
}


class PaddingValues {
    constructor()
    constructor(all: Dp)
    constructor(horizontal: Dp, vertical: Dp)
    constructor(start: Dp, top: Dp, end: Dp, bottom: Dp)
}

class Constraints {
    val hasBoundedHeight: Boolean get() = true
    val hasBoundedWidth: Boolean get() = true
}

@Composable
fun Box(modifier: Modifier = Modifier, content: @Composable () -> Unit) {}

class BoxWithConstraintsScope {
    val constraints: Constraints get() = Constraints()
    val maxHeight: Dp get() = Dp(0f)
    val maxWidth: Dp get() = Dp(0f)
}

@Composable
fun BoxWithConstraints(modifier: Modifier = Modifier, content: @Composable BoxWithConstraintsScope.() -> Unit) {}

/* Faithful to the real API (Compose source RowColumnImpl.kt): weight is a
 * MEMBER extension of RowScope/ColumnScope - never importable at top level
 * (an explicit `import androidx.compose.foundation.layout.weight` resolves
 * to the file-private `val RowColumnParentData?.weight` and K2 rejects it:
 * "Cannot access 'val RowColumnParentData?.weight: Float': it is internal
 * in file"). */
interface RowScope {
    fun Modifier.weight(weight: Float, fill: Boolean = true): Modifier = this
}
interface ColumnScope {
    fun Modifier.weight(weight: Float, fill: Boolean = true): Modifier = this
}

@Composable
fun Row(
    modifier: Modifier = Modifier,
    horizontalArrangement: Arrangement = Arrangement.Start,
    verticalAlignment: Alignment = Alignment.CenterVertically,
    content: @Composable RowScope.() -> Unit
) {}

@Composable
fun Column(
    modifier: Modifier = Modifier,
    verticalArrangement: Arrangement = Arrangement.Start,
    horizontalAlignment: Alignment = Alignment.CenterHorizontally,
    content: @Composable ColumnScope.() -> Unit
) {}

@Composable
fun Spacer(modifier: Modifier) {}

fun Modifier.fillMaxSize(): Modifier = this
fun Modifier.fillMaxSize(fraction: Float): Modifier = this
fun Modifier.fillMaxWidth(): Modifier = this
fun Modifier.fillMaxWidth(fraction: Float): Modifier = this
fun Modifier.fillMaxHeight(): Modifier = this
fun Modifier.height(height: Dp): Modifier = this
fun Modifier.heightIn(min: Dp = Dp.Unspecified, max: Dp = Dp.Unspecified): Modifier = this
fun Modifier.widthIn(min: Dp = Dp.Unspecified, max: Dp = Dp.Unspecified): Modifier = this
fun Modifier.imePadding(): Modifier = this
fun Modifier.navigationBarsPadding(): Modifier = this
fun Modifier.statusBarsPadding(): Modifier = this
fun Modifier.padding(paddingValues: PaddingValues): Modifier = this
fun Modifier.padding(all: Dp): Modifier = this
fun Modifier.padding(horizontal: Dp = Dp(0f), vertical: Dp = Dp(0f)): Modifier = this
fun Modifier.padding(start: Dp = Dp(0f), top: Dp = Dp(0f), end: Dp = Dp(0f), bottom: Dp = Dp(0f)): Modifier = this
