package android.database
interface Cursor : java.io.Closeable {
    fun moveToFirst(): Boolean
    fun getColumnIndex(column: String): Int
    fun getString(index: Int): String?
    fun getLong(index: Int): Long
    fun moveToNext(): Boolean
    override fun close() {}
}
