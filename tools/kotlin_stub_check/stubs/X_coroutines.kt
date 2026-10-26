package kotlinx.coroutines.flow
interface StateFlow<out T> {
    val value: T
}
class MutableStateFlow<T>(initial: T) : StateFlow<T> {
    override var value: T = initial
    fun update(function: (T) -> T) {}
}
fun <T> MutableStateFlow<T>.asStateFlow(): StateFlow<T> = this

fun <T> StateFlow<T>.update(function: (T) -> T) {}
