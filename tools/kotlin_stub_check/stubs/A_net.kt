package android.net
abstract class Uri {
    companion object {
        fun parse(value: String): Uri = throw UnsupportedOperationException("stub")
        fun fromFile(file: java.io.File): Uri = throw UnsupportedOperationException("stub")
    }
}
