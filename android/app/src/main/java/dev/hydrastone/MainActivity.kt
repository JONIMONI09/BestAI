package dev.hydrastone

import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import android.provider.OpenableColumns
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import java.io.File

/**
 * Hydra-Stone Android front end (programmatic Views, no Compose).
 *
 * Model selection has two paths:
 *  1. the demo model copied out of the APK assets (always present), and
 *  2. a user model imported through the Storage Access Framework.
 *
 * The import is a COPY into filesDir, streamed, written to a .tmp file and
 * then renamed. A copy is required because hydra_engine_load() mmaps a real
 * path, and a content:// URI is not one. Renaming after a complete write
 * means a cancelled or failed import can never leave a truncated model
 * behind. No persistable URI permission is taken: we never reopen the URI,
 * so it would only be a stale grant we do not need.
 */
class MainActivity : ComponentActivity() {

    private lateinit var output: TextView
    private lateinit var tokenInput: EditText
    private lateinit var stepsInput: EditText
    private lateinit var runButton: Button
    private lateinit var cancelButton: Button
    private var modelFile: File? = null

    /** True while an inference thread is alive. Gates Run/Cancel. */
    @Volatile
    private var running = false

    /** Log lines shown so far; capped so a 256-token run cannot grow without bound. */
    private val lineCount = StringBuilder()

    private val openModel =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            uri?.let { importModel(it) }
        }

    private fun log(line: String) {
        runOnUiThread {
            lineCount.append(line).append('\n')
            if (lineCount.length > MAX_LOG_CHARS) {
                lineCount.delete(0, lineCount.length - MAX_LOG_CHARS)
            }
            output.text = lineCount.toString()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        /* Crash handler first: anything that goes wrong from here on is
         * caught and shown with a copy button. */
        CrashHandler.appContext = applicationContext
        CrashHandler.install()

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

        val pick = Button(this).apply { setText(R.string.action_import_model) }
        root.addView(pick)

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
        runButton = run
        root.addView(run)

        /* Cancel is a real, cooperative cancel: it sets a flag the native
         * loop checks once per step (HydraBridge.cancel). It is disabled
         * while nothing runs, so it can never look like it does something
         * it does not. */
        val cancel = Button(this).apply { setText(R.string.action_cancel) }
        cancelButton = cancel
        cancel.isEnabled = false
        root.addView(cancel)

        val bench = Button(this).apply { setText(R.string.action_benchmark) }
        root.addView(bench)

        output = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 13f
        }
        root.addView(
            ScrollView(this).apply { addView(output) },
            LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f
            )
        )

        setContentView(root)

        // Copy the packed demo model out of assets to internal storage,
        // so the C loader can mmap it from a real filesystem path.
        modelFile = File(filesDir, "demo.hydra")
        if (!modelFile!!.exists()) {
            assets.open("demo.hydra").use { input ->
                modelFile!!.outputStream().use { output -> input.copyTo(output) }
            }
        }

        log(getString(R.string.log_model, modelFile!!.absolutePath))
        log(resources.getQuantityString(R.plurals.log_size, modelFile!!.length().toInt(), modelFile!!.length()))

        pick.setOnClickListener {
            // */* rather than a MIME filter: .hydra has no reliable MIME
            // mapping, and a filter would hide the file on most providers.
            openModel.launch(arrayOf("*/*"))
        }

        run.setOnClickListener { startInference() }
        cancel.setOnClickListener {
            /* Cooperative and asynchronous: the flag is set here, the
             * inference thread returns within one step. */
            HydraBridge.cancel()
            log(getString(R.string.log_cancel_armed))
        }
        bench.setOnClickListener { runBenchmark() }

        /* A report written during the previous session surfaces here, not
         * in the middle of the crash: the handler only writes the file and
         * delegates to the platform. */
        val pending = CrashHandler.pendingReport(filesDir)
        if (pending != null) {
            log(getString(R.string.crash_previous))
            startActivity(
                android.content.Intent(this, CrashActivity::class.java)
                    .putExtra(CrashActivity.EXTRA_REPORT, pending)
            )
        }

        maybeRunCrashTest()
    }

    private fun startInference() {
        if (running) return
        val startToken = tokenInput.text.toString().toIntOrNull() ?: 42
        val steps = (stepsInput.text.toString().toIntOrNull() ?: 32).coerceIn(1, 256)
        val file = modelFile ?: return
        lineCount.clear()
        setRunning(true)
        log(resources.getQuantityString(R.plurals.log_running, steps, startToken, steps))

        val t0 = SystemClock.elapsedRealtime()
        Thread {
            try {
                val json = HydraBridge.runInference(
                    file.absolutePath, intArrayOf(startToken), steps,
                    object : HydraBridge.Callback {
                        override fun onTokens(tokens: IntArray, done: Boolean) {
                            // One UI update per block, not per token: the old
                            // per-token callback rebuilt the whole TextView
                            // for every single token.
                            log(resources.getQuantityString(
                                R.plurals.log_tokens, tokens.size, tokens.size
                            ) + tokens.joinToString(", "))
                            if (done) log(getString(R.string.log_batch_done))
                        }

                        /* Only benchmark() still uses the per-token shape.
                         * A normal run must never take this path — if it
                         * did, the batching would be silently undone. */
                        override fun onToken(step: Int, token: Int) {
                            log(getString(R.string.log_unexpected_per_token, step, token))
                        }
                    }
                )
                val wall = SystemClock.elapsedRealtime() - t0
                /* A cancelled run is reported as cancelled, never as a
                 * failure: the user asked for it. */
                if (json.contains("\"cancelled\":true")) {
                    log(getString(R.string.log_cancelled))
                } else {
                    log(getString(R.string.log_result, json))
                    log(getString(R.string.log_wall, wall))
                    log(getString(R.string.log_ok))
                }
            } catch (e: Throwable) {
                log(getString(R.string.log_error, e.message))
            } finally {
                setRunning(false)
            }
        }.start()
    }

    /** Run and Cancel are mutually exclusive; only Cancel is live mid-run. */
    private fun setRunning(value: Boolean) {
        running = value
        runOnUiThread {
            runButton.isEnabled = !value
            cancelButton.isEnabled = value
        }
    }

    /**
     * Debug-only crash trigger: `adb shell am start -n dev.hydrastone/.MainActivity --ez crash_test true`
     *
     * It proves the whole chain - handler writes the file, platform kills
     * the process, next launch shows the report - which cannot be checked
     * without a device. Gated on FLAG_DEBUGGABLE so a release build cannot
     * be made to crash by an intent.
     */
    private fun maybeRunCrashTest() {
        if (!intent.getBooleanExtra(EXTRA_CRASH_TEST, false)) return
        if (applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE == 0) {
            log("[Hydra] crash_test ignored: this build is not debuggable")
            return
        }
        log("[Hydra] crash_test: throwing on a background thread on purpose")
        Thread { throw IllegalStateException("Hydra-Stone crash test (debug build only)") }
            .start()
    }

    /**
     * Phase 0 measurement on the device: native loop vs per-token JNI vs
     * batched JNI. The result is logged as raw JSON because every number
     * in it is one the reader can check against the code.
     */
    private fun runBenchmark() {
        val file = modelFile ?: return
        val steps = (stepsInput.text.toString().toIntOrNull() ?: 32).coerceIn(1, 200000)
        lineCount.clear()
        log(resources.getQuantityString(R.plurals.log_bench_start, steps, steps))

        Thread {
            try {
                val json = HydraBridge.benchmark(
                    file.absolutePath, steps,
                    object : HydraBridge.Callback {
                        override fun onTokens(tokens: IntArray, done: Boolean) = Unit
                        // Deliberately does nothing: the benchmark measures
                        // the CALLING CONVENTION, not the UI work. A real
                        // run updates a TextView per token, which is more
                        // expensive still - so these numbers are a floor.
                        override fun onToken(step: Int, token: Int) = Unit
                    }
                )
                log(getString(R.string.log_bench_result, json))
            } catch (e: Throwable) {
                log(getString(R.string.log_error, e.message))
            }
        }.start()
    }

    /**
     * Copy an imported model into filesDir, validate its 24-byte header and
     * only then make it the active model.
     *
     * The header check mirrors the C loader (include/hydra_model.h): magic,
     * version, dim/vocab bounds, layers cap, weights offset/length inside the
     * file and layers*dim covered by weights_len. Accepting a file the engine
     * would only reject later would leave the app with a model that fails on
     * every Run.
     */
    private fun importModel(uri: Uri) {
        val displayName = queryDisplayName(uri) ?: "imported.hydra"
        log(getString(R.string.log_import_start, displayName))

        Thread {
            try {
                val tmp = File(filesDir, "import.hydra.tmp")
                var bytes = 0L
                contentResolver.openInputStream(uri).use { input ->
                    if (input == null) throw IllegalStateException(getString(R.string.err_no_stream))
                    tmp.outputStream().use { out ->
                        val buf = ByteArray(64 * 1024)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            bytes += n
                            if (bytes > MAX_UPLOAD_BYTES) {
                                throw IllegalStateException(getString(R.string.err_too_large))
                            }
                            out.write(buf, 0, n)
                        }
                        out.fd.sync()
                    }
                }

                val error = validateHydraHeader(tmp, bytes)
                if (error != null) {
                    tmp.delete()
                    log(getString(R.string.log_import_rejected, error))
                    return@Thread
                }

                val target = File(filesDir, "imported.hydra")
                // Atomic within the same directory: readers see either the
                // old file or the complete new one, never a partial write.
                if (target.exists() && !target.delete()) {
                    tmp.delete()
                    log(getString(R.string.log_import_failed, getString(R.string.err_replace)))
                    return@Thread
                }
                if (!tmp.renameTo(target)) {
                    tmp.delete()
                    log(getString(R.string.log_import_failed, getString(R.string.err_rename)))
                    return@Thread
                }

                modelFile = target
                log(getString(R.string.log_import_ok, target.name))
                log(resources.getQuantityString(R.plurals.log_size, bytes.toInt(), bytes))
            } catch (e: Throwable) {
                log(getString(R.string.log_import_failed, e.message ?: "unknown"))
            }
        }.start()
    }

    /** Returns null when the file is a valid .hydra, otherwise the reason. */
    private fun validateHydraHeader(file: File, size: Long): String? {
        if (size < HEADER_BYTES) return getString(R.string.err_too_small)
        val header = ByteArray(HEADER_BYTES)
        file.inputStream().use { input ->
            if (input.read(header) != HEADER_BYTES) return getString(R.string.err_unreadable)
        }
        fun u16(off: Int) = (header[off].toInt() and 0xFF) or
            ((header[off + 1].toInt() and 0xFF) shl 8)
        fun u32(off: Int): Long =
            (header[off].toLong() and 0xFF) or
                ((header[off + 1].toLong() and 0xFF) shl 8) or
                ((header[off + 2].toLong() and 0xFF) shl 16) or
                ((header[off + 3].toLong() and 0xFF) shl 24)

        if (u32(0) != HYDRA_MAGIC) return getString(R.string.err_magic)
        if (u16(4) != HYDRA_VERSION) return getString(R.string.err_version)
        val vocab = u16(6)
        val dim = u32(8)
        val layers = u32(12)
        val offset = u32(16)
        val len = u32(20)

        if (dim == 0L || dim > MAX_DIM) return getString(R.string.err_dim)
        if (vocab == 0 || vocab > MAX_VOCAB) return getString(R.string.err_vocab)
        if (layers == 0L || layers > MAX_LAYERS) return getString(R.string.err_layers)
        if (offset < HEADER_BYTES) return getString(R.string.err_offset)
        if (offset + len > size) return getString(R.string.err_weights)
        if (layers * dim > len) return getString(R.string.err_pair_count)
        return null
    }

    private fun queryDisplayName(uri: Uri): String? =
        try {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { c ->
                    val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0 && c.moveToFirst()) c.getString(idx) else null
                }
        } catch (e: Throwable) {
            null
        }

    private companion object {
        const val EXTRA_CRASH_TEST = "crash_test"
        const val HEADER_BYTES = 24
        const val HYDRA_MAGIC = 0x48594452L
        const val HYDRA_VERSION = 1
        const val MAX_DIM = 64L
        const val MAX_VOCAB = 1024
        const val MAX_LAYERS = 4096L
        const val MAX_UPLOAD_BYTES = 64L * 1024 * 1024
        const val MAX_LOG_CHARS = 64 * 1024
    }
}