package android.app
import android.content.Context
open class Activity : Context() {
    var currentFocus: android.view.View? get() = null; set(v) {}
    fun finish() {}
    fun runOnUiThread(action: Runnable) {}
    open fun onCreate(savedInstanceState: android.os.Bundle?) {}
    open fun onDestroy() {}
    fun setResult(resultCode: Int) {}
    var isFinishing: Boolean get() = false; set(v) {}
    val intent: android.content.Intent get() = android.content.Intent(this)
    fun setContentView(view: android.view.View) {}
}
