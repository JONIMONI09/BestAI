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
            text = "Hydra-Stone — Android"
            textSize = 22f
        }
        root.addView(title)

        tokenInput = EditText(this).apply {
            hint = "start token"
            setText("42")
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
        }
        root.addView(tokenInput)

        stepsInput = EditText(this).apply {
            hint = "steps (1..256)"
            setText("32")
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
        }
        root.addView(stepsInput)

        val run = Button(this).apply { text = "Run inference" }
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

        log("[Hydra] model: ${modelFile!!.absolutePath}")
        log("[Hydra] size: ${modelFile!!.length()} bytes")

        run.setOnClickListener {
            val startToken = tokenInput.text.toString().toIntOrNull() ?: 42
            val steps = (stepsInput.text.toString().toIntOrNull() ?: 32)
                .coerceIn(1, 256)
            lineCount.clear()
            log("[Hydra] running: startToken=$startToken steps=$steps ...")
            val t0 = SystemClock.elapsedRealtime()
            Thread {
                try {
                    val json = HydraBridge.runInference(
                        modelFile!!.absolutePath, startToken, steps,
                        object : HydraBridge.Callback {
                            override fun onToken(step: Int, token: Int) {
                                if (step < 8) log("[Hydra] token[$step] = $token")
                            }
                        }
                    )
                    val wall = SystemClock.elapsedRealtime() - t0
                    log("[Hydra] result: $json")
                    log("[Hydra] wall time: $wall ms")
                    log("[Hydra] OK — C engine executed on Android.")
                } catch (e: Throwable) {
                    log("[Hydra] ERROR: ${e.message}")
                }
            }.start()
        }
    }
}
