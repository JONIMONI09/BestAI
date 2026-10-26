package androidx.activity.result.contract
open class ActivityResultContract<I, O>()
object ActivityResultContracts {
    class OpenDocument : ActivityResultContract<Array<String>?, android.net.Uri?>()
}
