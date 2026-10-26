package android.widget
open class LinearLayout(context: android.content.Context) : android.view.View(context) {
    constructor(context: android.content.Context, attrs: Int?) : this(context)
    var orientation: Int get() = 0; set(v) {}
    var gravity: Int get() = 0; set(v) {}  // LinearLayout-specific: View has no gravity member
    fun addView(child: android.view.View) {}
    fun addView(child: android.view.View, params: LayoutParams?) {}
    class LayoutParams(width: Int, height: Int, weight: Float = 0f) {
        companion object {
            const val MATCH_PARENT = -1
            const val WRAP_CONTENT = -2
        }
    }
    companion object {
        const val HORIZONTAL = 0
        const val VERTICAL = 1
    }
}
open class ScrollView(context: android.content.Context) : android.view.View(context) {
    constructor(context: android.content.Context, attrs: Int?) : this(context)
    fun setFillViewport(fill: Boolean) {}
    fun addView(child: android.view.View) {}
}
open class TextView(context: android.content.Context) : android.view.View(context) {
    constructor(context: android.content.Context, attrs: Int?) : this(context)
    constructor(context: android.content.Context, attrs: Int?, defStyle: Int) : this(context)
    fun setText(resId: Int) {}
    fun setTextIsSelectable(selectable: Boolean) {}
    var gravity: Int get() = 0; set(v) {}
    var text: CharSequence get() = ""; set(v) {}
    var typeface: android.graphics.Typeface? get() = null; set(v) {}
    var textSize: Float get() = 0f; set(v) {}
    var maxLines: Int get() = 0; set(v) {}
    fun setTextColor(color: Int) {}
    fun setLineSpacing(add: Float, mult: Float) {}
}
class Button(context: android.content.Context) : TextView(context) {
    constructor(context: android.content.Context, attrs: Int?) : this(context)
    constructor(context: android.content.Context, attrs: Int?, defStyle: Int) : this(context)
}
