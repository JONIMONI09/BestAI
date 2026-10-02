package dev.hydrastone

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * Shows a crash report: copy it, or delete it.
 *
 * This activity is NOT started by [CrashHandler]. It is started by
 * MainActivity on the next launch when a report file exists - starting an
 * activity from an uncaught-exception handler is unreliable, because the
 * process is already dying and the request usually never lands.
 *
 * The report text comes either from [EXTRA_REPORT] or, when that is absent,
 * from the stored file, so the same screen serves both the "here is the
 * report you just asked for" and the "here is what happened last time"
 * case.
 */
class CrashActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val fromIntent = intent.getStringExtra(EXTRA_REPORT)
        val report = fromIntent ?: CrashHandler.pendingReport(filesDir).orEmpty()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
        }

        root.addView(TextView(this).apply {
            setText(R.string.crash_title)
            textSize = 20f
        })
        root.addView(TextView(this).apply {
            setText(R.string.crash_explanation)
            textSize = 13f
            setPadding(0, 16, 0, 16)
        })

        val detail = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 11f
            text = report.ifEmpty { getString(R.string.crash_no_report) }
            setTextIsSelectable(true)
            gravity = Gravity.START
        }
        root.addView(
            ScrollView(this).apply { addView(detail) },
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f)
        )

        val actions = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.END
            setPadding(0, 24, 0, 0)
        }
        val copy = Button(this).apply { setText(R.string.crash_copy) }
        copy.tag = "crash-copy-button"
        val delete = Button(this).apply { setText(R.string.crash_delete) }
        delete.tag = "crash-delete-button"
        val close = Button(this).apply { setText(R.string.crash_close) }
        actions.addView(copy)
        actions.addView(delete)
        actions.addView(close)

        copy.setOnClickListener {
            val ok = CrashHandler.copyToClipboard(this, report)
            copy.text = getString(if (ok) R.string.crash_copied else R.string.crash_copy_failed)
        }
        /* Deleting is destructive and irreversible, so it is labelled as
         * such and does not happen by closing the screen. */
        delete.setOnClickListener {
            CrashHandler.clearReport(filesDir)
            delete.text = getString(R.string.crash_deleted)
            delete.isEnabled = false
        }
        close.setOnClickListener { finish() }

        root.addView(actions)
        setContentView(root)
    }

    companion object {
        const val EXTRA_REPORT = "report"
    }
}