package org.json
class JSONObject(json: String?) {
    fun optBoolean(name: String, fallback: Boolean = false): Boolean = fallback
    fun optInt(name: String, fallback: Int = 0): Int = fallback
    fun optLong(name: String, fallback: Long = 0L): Long = fallback
    fun optString(name: String, fallback: String = ""): String = fallback
}
