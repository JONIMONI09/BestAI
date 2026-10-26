package androidx.compose.foundation
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier

class ScrollState(val initialValue: Int = 0)
@Composable
fun rememberScrollState(initialValue: Int = 0): ScrollState = ScrollState(initialValue)
fun Modifier.verticalScroll(state: ScrollState, enabled: Boolean = true, reverseLayout: Boolean = false): Modifier = this
