package dev.hydrastone

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Build
import android.util.Log
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter

/**
 * Persists every uncaught exception to a file and then hands the crash to
 * the platform.
 *
 * The order is the whole point, and it is the opposite of what this class
 * used to do:
 *
 *  1. WRITE the report to [REPORT_FILE] in filesDir. This is the only step
 *     that survives: it touches nothing but the filesystem, needs no window,
 *     no ActivityManager, and no permission.
 *  2. DELEGATE to the handler that was installed before us, so Android
 *     still shows its dialog, writes a tombstone and terminates the process
 *     the way the platform expects.
 *
 * Starting an Activity from the handler is what the old version did, and it
 * is unreliable by construction: the handler runs on the crashing thread,
 * the process is usually milliseconds from death, and the ActivityManager
 * may simply drop the request. The report therefore shows up on the NEXT
 * launch instead - see [pendingReport], which MainActivity checks.
 *
 * Logs stay free of user content on purpose: the report carries the thread,
 * the stack, the version and the device, never a prompt or model bytes.
 */
object CrashHandler : Thread.UncaughtExceptionHandler {

    private const val TAG = "HydraCrash"
    private const val MAX_REPORT_BYTES = 64 * 1024

    /** Name of the stored report, relative to filesDir. */
    const val REPORT_FILE = "last-crash.txt"

    @Volatile
    private var previous: Thread.UncaughtExceptionHandler? = null

    fun install() {
        if (previous != null) return
        previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler(this)
    }

    override fun uncaughtException(thread: Thread, error: Throwable) {
        try {
            val report = buildReport(thread, error)
            val dir = reportDir()
            if (dir != null) {
                writeReport(dir, report)
                Log.e(TAG, "crash report written to ${File(dir, REPORT_FILE).absolutePath}")
            } else {
                // No filesDir yet (a crash before Application.onCreate):
                // the platform handler still gets the stack trace via
                // Log, so the bug is not lost entirely.
                Log.e(TAG, report)
            }
        } catch (secondary: Throwable) {
            // A crash handler that crashes is worse than no crash handler.
            Log.e(TAG, "crash handler failed", secondary)
        } finally {
            // Always hand the crash to the platform, otherwise the app
            // limps on in an undefined state and the bug disappears.
            previous?.uncaughtException(thread, error)
        }
    }

    /** filesDir, or null when there is no Context yet (never guess /sdcard). */
    private fun reportDir(): File? {
        val dir = appContext?.filesDir ?: return null
        if (!dir.exists()) dir.mkdirs()
        return if (dir.isDirectory) dir else null
    }

    /**
     * Builds the report text.
     *
     * Split out from [uncaughtException] so it can be reasoned about (and
     * reused) without a Context: it only reads [error] and the arguments.
     */
    fun buildReport(
        thread: Thread,
        error: Throwable,
        whenMillis: Long = System.currentTimeMillis(),
        device: String = "${Build.DEVICE} / ${Build.PRODUCT}",
        abi: String = Build.SUPPORTED_ABIS.joinToString(),
        androidRelease: String = Build.VERSION.RELEASE,
        apiLevel: Int = Build.VERSION.SDK_INT
    ): String {
        val sw = StringWriter()
        error.printStackTrace(PrintWriter(sw))
        return buildString {
            appendLine("Hydra-Stone crash report")
            appendLine("time: ${java.util.Date(whenMillis)}")
            appendLine("android: $androidRelease (API $apiLevel)")
            appendLine("device: $device")
            appendLine("abi: $abi")
            appendLine("thread: ${thread.name}")
            appendLine()
            append(sw.toString())
        }.take(MAX_REPORT_BYTES)
    }

    /** Writes the report, replacing any previous one. Returns success. */
    fun writeReport(dir: File, report: String): Boolean = try {
        val target = File(dir, REPORT_FILE)
        // Write to a temporary name and rename: a process killed halfway
        // through the write must not leave a truncated report that looks
        // like a complete one.
        val tmp = File(dir, "$REPORT_FILE.tmp")
        tmp.writeText(report)
        if (!tmp.renameTo(target)) {
            tmp.delete()
            false
        } else {
            true
        }
    } catch (e: Throwable) {
        Log.e(TAG, "could not write the crash report", e)
        false
    }

    /**
     * The stored report, or null when there is none.
     *
     * MainActivity calls this on every start: a report that was written
     * during the previous session shows up here instead of at the moment
     * of the crash.
     */
    fun pendingReport(dir: File): String? = try {
        val f = File(dir, REPORT_FILE)
        if (f.isFile && f.length() > 0) f.readText() else null
    } catch (e: Throwable) {
        Log.e(TAG, "could not read the crash report", e)
        null
    }

    /** Removes the stored report. Called when the user deletes it. */
    fun clearReport(dir: File): Boolean {
        val tmp = File(dir, "$REPORT_FILE.tmp")
        tmp.delete()
        return File(dir, REPORT_FILE).delete()
    }

    fun copyToClipboard(context: Context, text: String): Boolean = try {
        val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("Hydra-Stone crash", text))
        true
    } catch (e: Throwable) {
        Log.e(TAG, "clipboard unavailable", e)
        false
    }

    @Volatile
    var appContext: Context? = null
}