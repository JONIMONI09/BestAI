package androidx.activity
import android.app.Activity
open class ComponentActivity : Activity() {
    fun <I, O> registerForActivityResult(
        contract: androidx.activity.result.contract.ActivityResultContract<I, O>,
        callback: (O) -> Unit
    ): androidx.activity.result.ActivityResultLauncher<I> = throw UnsupportedOperationException("stub")
}
fun ComponentActivity.enableEdgeToEdge() {}
