package dev.hydrastone

import android.app.Activity
import android.os.Bundle
import android.os.SystemClock
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File

class MainActivity : Activity() {

    private lateinit var output: TextView
    private lateinit var tokenInput: EditText
    private lateinit var stepsInput: EditText
    private var modelFile: File? = null

    private val lineCount = StringBuilder()

    private fun log(line: String) {
        runOnUiThread {
            lineCount.append(line).append('\n')
            output.text = lineCount.toString()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
        }

        val title = TextView(this).apply {
            // String aus den Ressourcen, nicht hart kodiert (Android Lint SetTextI18n)
            setText(R.string.app_title)
            textSize = 22f
        }
        root.addView(title)

        tokenInput = EditText(this).apply {
            hint = getString(R.string.hint_start_token)
            setText(R.string.default_start_token)
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
        }
        root.addView(tokenInput)

        stepsInput = EditText(this).apply {
            hint = getString(R.string.hint_steps)
            setText(R.string.default_steps)
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
        }
        root.addView(stepsInput)

        val run = Button(this).apply { setText(R.string.action_run) }
        root.addView(run)

        output = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 13f
        }
        root.addView(ScrollView(this).apply {
            addView(output)
        }, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f
        ))

        setContentView(root)

        // Copy the packed demo model out of assets to internal storage,
        // so the C loader can mmap it from a real filesystem path.
        modelFile = File(filesDir, "demo.hydra")
        if (!modelFile!!.exists()) {
            assets.open("demo.hydra").use { input ->
                modelFile!!.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
        }

        log(getString(R.string.log_model, modelFile!!.absolutePath))
        log(resources.getQuantityString(R.plurals.log_size, modelFile!!.length().toInt(), modelFile!!.length()))

        run.setOnClickListener {
            val startToken = tokenInput.text.toString().toIntOrNull() ?: 42
            val steps = (stepsInput.text.toString().toIntOrNull() ?: 32)
                .coerceIn(1, 256)
            lineCount.clear()
            log(resources.getQuantityString(R.plurals.log_running, steps, startToken, steps))
            val t0 = SystemClock.elapsedRealtime()
            Thread {
                try {
                    val json = HydraBridge.runInference(
                        modelFile!!.absolutePath, startToken, steps,
                        object : HydraBridge.Callback {
                            override fun onToken(step: Int, token: Int) {
                                if (step < 8) log(getString(R.string.log_token, step, token))
                            }
                        }
                    )
                    val wall = SystemClock.elapsedRealtime() - t0
                    log(getString(R.string.log_result, json))
                    log(getString(R.string.log_wall, wall))
                    log(getString(R.string.log_ok))
                } catch (e: Throwable) {
                    log(getString(R.string.log_error, e.message))
                }
            }.start()
        }
    }
}