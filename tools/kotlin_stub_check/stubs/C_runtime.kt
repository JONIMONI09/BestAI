package androidx.compose.runtime
import kotlin.reflect.KProperty
import kotlinx.coroutines.flow.StateFlow

@Target(AnnotationTarget.FUNCTION, AnnotationTarget.TYPE, AnnotationTarget.TYPE_PARAMETER, AnnotationTarget.VALUE_PARAMETER)
annotation class Composable

interface State<out T> {
    val value: T
}

class MutableState<T>(initial: T) : State<T> {
    override var value: T = initial
}

fun <T> mutableStateOf(value: T): MutableState<T> = MutableState(value)

@Suppress("UNCHECKED_CAST")
inline fun <T> remember(vararg keys: Any?, calculation: @Composable () -> T): T =
    throw UnsupportedOperationException("stub")

@Composable
fun LaunchedEffect(vararg keys: Any?, block: suspend () -> Unit) {}

operator fun <T> State<T>.getValue(thisObj: Any?, property: KProperty<*>): T = value

operator fun <T> MutableState<T>.setValue(thisObj: Any?, property: KProperty<*>, newValue: T) {
    value = newValue
}

@Composable
fun <T> StateFlow<T>.collectAsState(): State<T> = throw UnsupportedOperationException("stub")
