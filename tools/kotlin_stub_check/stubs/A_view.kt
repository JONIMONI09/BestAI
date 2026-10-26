package android.view
open class View(context: android.content.Context) {
    fun setOnClickListener(listener: OnClickListener?) {}
    var tag: Any? get() = null; set(v) {}
    var isEnabled: Boolean get() = true; set(v) {}
    fun setPadding(left: Int, top: Int, right: Int, bottom: Int) {}
    fun setBackgroundRes(id: Int) {}
    val windowToken: android.os.IBinder? get() = null
    fun clearFocus() {}
    fun requestFocus(): Boolean = true
    fun interface OnClickListener { fun onClick(v: View?) }
}
object Gravity {
    const val CENTER_HORIZONTAL = 1
    const val CENTER_VERTICAL = 16
    const val START = 8355712
    const val END = 8355715
    const val TOP = 48
    const val BOTTOM = 80
}
