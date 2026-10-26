package android.content
import java.io.File
open class Context {
    open fun getSharedPreferences(name: String, mode: Int): SharedPreferences = throw UnsupportedOperationException("stub")
    fun getPreferences(mode: Int): SharedPreferences = throw UnsupportedOperationException("stub")
    fun getSystemService(name: String): Any? = null
    open fun getString(resId: Int): String = throw UnsupportedOperationException("stub")
    open fun getString(resId: Int, vararg formatArgs: Any?): String = throw UnsupportedOperationException("stub")
    fun startActivity(intent: Intent) {}
    open val filesDir: File get() = throw UnsupportedOperationException("stub")
    open val resources: Resources get() = throw UnsupportedOperationException("stub")
    open val applicationInfo: android.content.pm.ApplicationInfo get() = throw UnsupportedOperationException("stub")
    open val applicationContext: Context get() = this
    open val assets: AssetManager get() = AssetManager()
    class AssetManager { fun open(name: String): java.io.InputStream = throw UnsupportedOperationException("stub") }
    open val contentResolver: ContentResolver get() = throw UnsupportedOperationException("stub")
    companion object {
        const val MODE_PRIVATE = 0
        const val INPUT_METHOD_SERVICE = "input_method"
        const val CLIPBOARD_SERVICE = "clipboard"
    }
}
class SharedPreferences {
    fun edit(): Editor = Editor()
    fun getBoolean(key: String, defValue: Boolean): Boolean = defValue
    fun getString(key: String, defValue: String?): String? = defValue
    fun getFloat(key: String, defValue: Float): Float = defValue
    fun getInt(key: String, defValue: Int): Int = defValue
    fun contains(key: String): Boolean = false
    inner class Editor {
        fun putBoolean(key: String, value: Boolean): Editor = this
        fun putString(key: String, value: String?): Editor = this
        fun putFloat(key: String, value: Float): Editor = this
        fun putInt(key: String, value: Int): Editor = this
        fun remove(key: String): Editor = this
        fun apply() {}
        fun commit(): Boolean = true
    }
}
class Resources {
    fun getQuantityString(id: Int, quantity: Int): String = ""
    fun getQuantityString(id: Int, quantity: Int, vararg formatArgs: Any): String = ""
    fun getString(id: Int): String = ""
    fun getString(id: Int, vararg formatArgs: Any?): String = ""
}
class Intent(context: Context) {
    constructor(context: Context, cls: Class<*>) : this(context)
    fun putExtra(name: String, value: String): Intent = this
    fun putExtra(name: String, value: Int): Intent = this
    fun putExtra(name: String, value: Boolean): Intent = this
    fun getBooleanExtra(name: String, defaultValue: Boolean): Boolean = defaultValue
    fun getStringExtra(name: String): String? = null
    fun setData(data: android.net.Uri?): Intent = this
    fun addFlags(flags: Int): Intent = this
    fun getDataType(): String? = null
    companion object {
        const val ACTION_VIEW = "android.intent.action.VIEW"
        const val FLAG_GRANT_READ_URI_PERMISSION = 1
        const val FLAG_GRANT_WRITE_URI_PERMISSION = 2
    }
}
abstract class ContentResolver {
    fun openInputStream(uri: android.net.Uri): java.io.InputStream? = null
    fun query(uri: android.net.Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): android.database.Cursor? = null
}
class ClipboardManager {
    fun setPrimaryClip(clip: ClipData) {}
}
class ClipData {
    companion object {
        fun newPlainText(label: CharSequence, text: CharSequence): ClipData = throw UnsupportedOperationException("stub")
    }
}
