package dev.hydrastone

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * Shows a crash report with a copy button.
 *
 * A separate activity (rather than an AlertDialog inside MainActivity)
 * because the crash may have killed MainActivity itself — this one starts
 * from a plain Context with FLAG_ACTIVITY_NEW_TASK and does not depend on
 * anything that was already initialised.
 */
class CrashActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val report = intent.getStringExtra(EXTRA_REPORT).orEmpty()

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
        val close = Button(this).apply { setText(R.string.crash_close) }
        actions.addView(copy)
        actions.addView(close)

        copy.setOnClickListener {
            val ok = CrashHandler.copyToClipboard(this, report)
            copy.text = getString(if (ok) R.string.crash_copied else R.string.crash_copy_failed)
        }
        close.setOnClickListener { finish() }

        root.addView(actions)
        setContentView(root)
    }

    companion object {
        const val EXTRA_REPORT = "report"
    }
}