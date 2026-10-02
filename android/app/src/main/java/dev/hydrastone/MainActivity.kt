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
     * The header check mirrors the C loader (include/hydra_model.h) rule for
     * rule - the same rules the web console reports through
     * GET /api/models/inspect - so a file rejected here is refused there for
     * the same stated reason. Accepting a file the engine would only reject
     * later would leave the app with a model that fails on every Run.
     *
     * On rejection every violated rule is logged, not just the first one: a
     * user who picked the wrong file needs the whole list, and "wrong magic"
     * on its own told nobody what they actually held.
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
                                throw IllegalStateException(getString(R.string.err_too_large_human, formatBytes(MAX_UPLOAD_BYTES)))
                            }
                            out.write(buf, 0, n)
                        }
                        out.fd.sync()
                    }
                }

                val report = analyseHydraFile(tmp, bytes)
                if (!report.valid) {
                    tmp.delete()
                    // The header itself, so the user can see what they picked.
                    log(getString(R.string.log_import_header, report.describe()))
                    for (rule in report.rules.filter { !it.ok }) {
                        log("  - ${rule.id}: ${rule.message}")
                    }
                    log(getString(R.string.log_import_rejected, report.firstViolation()))
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

    /**
     * The result of a header check: the values that were read plus one entry
     * per rule with its verdict.
     */
    private data class HeaderRule(val id: String, val ok: Boolean, val message: String)

    private data class HeaderReport(
        val bytes: Long,
        val fields: Map<String, Long> = emptyMap(),
        val detected: String? = null,
        val advice: String? = null,
        val rules: List<HeaderRule> = emptyList()
    ) {
        val valid: Boolean get() = rules.all { it.ok }

        /** Every violated rule, joined - the useful part of a long list. */
        fun firstViolation(): String =
            rules.firstOrNull { !it.ok }?.message ?: "model rejected"

        fun describe(): String = if (fields.isEmpty()) {
            "$bytes bytes"
        } else {
            fields.entries.joinToString(" ") { (k, v) ->
                if (k == "magic") "$k=${hex32(v)}" else "$k=$v"
            } + " ($bytes bytes)" + (detected?.let { ", detected: $it" } ?: "")
        }
    }

    /**
     * Magics worth naming. "wrong magic 0x46554747" tells nobody that they
     * just handed the app a llama.cpp model; this does.
     */
    private fun describeMagic(magic: Long): Pair<String?, String?> = when {
        magic == HYDRA_MAGIC -> "HYDRA" to null
        magic == GGUF_MAGIC -> "GGUF" to getString(R.string.err_magic_gguf)
        magic == ZIP_MAGIC -> "ZIP archive" to getString(R.string.err_magic_zip)
        magic == ELF_MAGIC -> "ELF binary" to getString(R.string.err_magic_elf)
        magic == PNG_MAGIC -> "PNG image" to getString(R.string.err_magic_png)
        (magic and 0xFFFFL) == GZIP_MAGIC -> "gzip archive" to getString(R.string.err_magic_gzip)
        else -> null to getString(R.string.err_magic_unknown, hex32(magic))
    }

    /** 1048576 -> "1.0 MiB". The same phrasing the web console uses. */
    private fun formatBytes(n: Long): String {
        val units = arrayOf("B", "KiB", "MiB", "GiB", "TiB")
        var v = n.toDouble()
        var i = 0
        while (v >= 1024 && i < units.size - 1) {
            v /= 1024
            i += 1
        }
        /* Locale.ROOT: a device set to Turkish would otherwise render the
         * unit list differently, and Android Lint is right about that. */
        return if (i == 0) "${v.toLong()} ${units[i]}"
        else String.format(java.util.Locale.ROOT, "%.1f %s", v, units[i])
    }

    /**
     * Checks a .hydra header the same way the C loader and the web console
     * do, and reports EVERY rule instead of returning on the first failure.
     *
     * Rule ids match the server's /api/models/inspect report on purpose:
     * "dim" here and "dim" there are the same rule, so a user comparing the
     * two sees one story, not two.
     */
    private fun analyseHydraFile(file: File, size: Long): HeaderReport {
        val rules = mutableListOf<HeaderRule>()
        if (size < HEADER_BYTES) {
            rules += HeaderRule("size", false, getString(R.string.err_too_small_human, size))
            return HeaderReport(size, rules = rules)
        }

        val header = ByteArray(HEADER_BYTES)
        file.inputStream().use { input ->
            if (input.read(header) != HEADER_BYTES) {
                rules += HeaderRule("header_readable", false, getString(R.string.err_unreadable))
                return HeaderReport(size, rules = rules)
            }
        }
        rules += HeaderRule("header_readable", true, "$HEADER_BYTES-byte header read")

        fun u16(off: Int): Long = (header[off].toLong() and 0xFF) or
            ((header[off + 1].toLong() and 0xFF) shl 8)
        fun u32(off: Int): Long =
            (header[off].toLong() and 0xFF) or
                ((header[off + 1].toLong() and 0xFF) shl 8) or
                ((header[off + 2].toLong() and 0xFF) shl 16) or
                ((header[off + 3].toLong() and 0xFF) shl 24)

        val magic = u32(0)
        val version = u16(4)
        val vocab = u16(6)
        val dim = u32(8)
        val layers = u32(12)
        val offset = u32(16)
        val len = u32(20)
        val fields = mapOf(
            "magic" to magic, "version" to version, "vocab" to vocab,
            "dim" to dim, "layers" to layers, "weights_offset" to offset,
            "weights_len" to len
        )

        val (detected, advice) = describeMagic(magic)
        if (magic != HYDRA_MAGIC) {
            rules += HeaderRule(
                "magic", false,
                advice ?: getString(R.string.err_magic_unknown, hex32(magic))
            )
            return HeaderReport(size, fields, detected, advice, rules)
        }
        rules += HeaderRule("magic", true, "magic matches .hydra")

        rules += HeaderRule(
            "version", version == HYDRA_VERSION,
            getString(
                if (version == HYDRA_VERSION) R.string.ok_version
                else R.string.err_version_human, version, HYDRA_VERSION
            )
        )
        rules += HeaderRule(
            "dim", dim >= 1 && dim <= MAX_DIM,
            getString(R.string.err_dim_human, dim, MAX_DIM)
        )
        rules += HeaderRule(
            "vocab", vocab >= 1 && vocab <= MAX_VOCAB,
            getString(R.string.err_vocab_human, vocab, MAX_VOCAB)
        )
        rules += HeaderRule(
            "layers", layers >= 1 && layers <= MAX_LAYERS,
            getString(R.string.err_layers_human, layers, MAX_LAYERS)
        )
        rules += HeaderRule(
            "weights_offset", offset >= HEADER_BYTES,
            getString(R.string.err_offset_human, offset, HEADER_BYTES)
        )
        /* Long arithmetic on purpose: offset+len in Int would wrap exactly
         * like the crafted-header bug the engine test pins. */
        val weightsEnd = offset + len
        rules += HeaderRule(
            "weights_fit", weightsEnd <= size,
            getString(R.string.err_weights_human, formatBytes(weightsEnd), formatBytes(size))
        )
        val pairs = layers * dim
        rules += HeaderRule(
            "pairs_covered", pairs <= len,
            getString(R.string.err_pair_count_human, pairs, len / 4)
        )
        /* Informational: how much of the current format's ceiling this model
         * uses. The format caps at dim x layers pairs, which is far below a
         * modern LLM - worth saying before somebody wonders. */
        val shapeBytes = (pairs + 1) / 2
        val ceiling = (MAX_DIM * MAX_LAYERS + 1) / 2
        rules += HeaderRule(
            "shape_within_format_ceiling", shapeBytes <= ceiling,
            getString(R.string.info_shape_ceiling, formatBytes(shapeBytes), formatBytes(ceiling))
        )

        return HeaderReport(size, fields, detected, advice, rules)
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
        const val HYDRA_VERSION = 1L

        /** In the companion so the nested [HeaderReport] can format too. */
        fun hex32(v: Long): String =
            "0x" + (v and 0xFFFFFFFFL).toString(16).padStart(8, '0')
        const val GGUF_MAGIC = 0x46554747L
        const val ZIP_MAGIC = 0x04034B50L
        const val ELF_MAGIC = 0x7F454C46L
        const val PNG_MAGIC = 0x89504E47L
        const val GZIP_MAGIC = 0x1F8BL
        const val MAX_DIM = 64L
        const val MAX_VOCAB = 1024
        const val MAX_LAYERS = 4096L
        const val MAX_UPLOAD_BYTES = 64L * 1024 * 1024
        const val MAX_LOG_CHARS = 64 * 1024
    }
}