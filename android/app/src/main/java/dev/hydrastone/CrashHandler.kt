package dev.hydrastone

import android.app.Activity
import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter

/**
 * Catches every uncaught exception on every thread, writes a report next to
 * the app's files and shows a dialog whose whole content can be copied to
 * the clipboard.
 *
 * Why a file *and* a dialog: a crash during startup kills the activity, so
 * the dialog may never appear — the file is what survives. Why copy instead
 * of a share intent: sharing needs an activity that is still alive and a
 * chooser the user has to drive; copying works even while the process is
 * already tearing down.
 */
object CrashHandler : Thread.UncaughtExceptionHandler {

    private const val TAG = "HydraCrash"
    private const val MAX_REPORT_BYTES = 64 * 1024

    @Volatile
    private var previous: Thread.UncaughtExceptionHandler? = null

    fun install() {
        previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler(this)
    }

    override fun uncaughtException(thread: Thread, error: Throwable) {
        try {
            val report = buildReport(thread, error)
            File(activityFilesDir(), "last-crash.txt").writeText(report)
            Log.e(TAG, report)
            showDialog(report)
        } catch (secondary: Throwable) {
            // A crash handler that crashes is worse than no crash handler.
            Log.e(TAG, "crash handler failed", secondary)
        } finally {
            // Always hand the crash to the platform, otherwise the app
            // limps on in an undefined state and the bug disappears.
            previous?.uncaughtException(thread, error)
        }
    }

    private fun activityFilesDir(): File {
        val dir = CrashHandler.appContext?.filesDir
            ?: File(System.getProperty("java.io.tmpdir") ?: "/data/local/tmp")
        if (!dir.exists()) dir.mkdirs()
        return dir
    }

    private fun buildReport(thread: Thread, error: Throwable): String {
        val sw = StringWriter()
        error.printStackTrace(PrintWriter(sw))
        return buildString {
            appendLine("Hydra-Stone crash report")
            appendLine("time: ${java.util.Date()}")
            appendLine("android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
            appendLine("device: ${Build.DEVICE} / ${Build.PRODUCT}")
            appendLine("abi: ${Build.SUPPORTED_ABIS.joinToString()}")
            appendLine("thread: ${thread.name}")
            appendLine()
            append(sw.toString())
        }.take(MAX_REPORT_BYTES)
    }

    private fun showDialog(report: String) {
        val ctx = appContext ?: return
        ctx.startActivity(
            Intent(ctx, CrashActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                .putExtra(CrashActivity.EXTRA_REPORT, report)
        )
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