package dev.hydrastone

import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import android.provider.OpenableColumns
import android.widget.Button
import android.widget.CheckBox
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
 *  1. the starter model shipped in the APK assets (always present, a real
 *     working .hydra so the engine has something to run), and
 *  2. a user model imported through the Storage Access Framework, which
 *     takes precedence on every launch after the first import.
 *
 * The app RUNS BY ITSELF. On every launch it prepares a model and starts a
 * generation run without waiting for a button press; "Run inference" re-runs
 * it, and the auto-run switch turns the automatic run off. Two things made
 * that fail before, and both are fixed here rather than papered over:
 *
 *  - the asset copy used to sit in onCreate() unguarded, so a missing or
 *    unreadable asset threw on the main thread, killed the activity and
 *    left nothing on screen at all. Model preparation is now a function
 *    that returns false and says why.
 *  - nothing started a run until the user pressed the button, which read
 *    as "the app does nothing" on a phone.
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
    private lateinit var modelLabel: TextView
    private var modelFile: File? = null

    /** Run automatically on launch. Off = the app waits for Run inference. */
    @Volatile
    private var autoRun = true

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
        /* Mirrored to logcat under the tag "Hydra".
         *
         * The TextView is the user-facing view, but it is also the only place
         * this output used to live, and reading it back on a headless
         * emulator needs uiautomator - which under TCG software emulation is
         * heavy enough to starve the guest and kill it. logcat is the
         * surface a CI machine can actually read cheaply. */
        android.util.Log.i(LOG_TAG, line)
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

        /* The whole screen scrolls.
         *
         * The root used to be a bare LinearLayout. On a 320x640 device the
         * controls (title, import, two inputs, run, cancel, benchmark,
         * model label, auto-run box) do not fit, and a vertical LinearLayout
         * lays the remaining children out BELOW the bottom edge instead of
         * making them reachable. The token log went with them, so the app
         * ran the engine and showed nothing - indistinguishable from "it
         * does not run". Confirmed on the emulator: a UI dump of the
         * running app ended at "Cancel run", with no log view at all.
         *
         * One ScrollView around everything, and the log gets a guaranteed
         * minimum height so it is readable even on a short screen. */
        val pad = (16 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
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

        /* Which model is actually loaded, stated in the UI. "Something runs"
         * is not the same as "you can see what ran". */
        modelLabel = TextView(this).apply { textSize = 12f }
        root.addView(modelLabel)

        val auto = CheckBox(this).apply {
            setText(R.string.action_auto_run)
            isChecked = true
        }
        root.addView(auto)

        output = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 13f
            /* Enough rows for a real run even when the screen is short;
             * the outer ScrollView handles the rest. */
            minLines = 8
            setBackgroundColor(0x11000000)
            setPadding(pad / 2, pad / 2, pad / 2, pad / 2)
        }
        root.addView(output)

        setContentView(ScrollView(this).apply { addView(root) })

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
        auto.setOnCheckedChangeListener { _, checked -> autoRun = checked }

        /* Model preparation FIRST, and guarded. An exception here used to
         * escape onCreate() and kill the activity before anything was drawn
         * - the app simply did not come up. */
        if (!prepareModel()) {
            log(getString(R.string.err_no_model))
            runButton.isEnabled = false
            cancelButton.isEnabled = false
            modelLabel.setText(R.string.err_no_model)
            return
        }

        /* Und dann laeuft es von selbst. */
        if (autoRun) {
            log(getString(R.string.log_auto_run))
            root.post { startInference() }
        }

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

    /**
     * Put a usable model in place, or say why there is none.
     *
     * A previously imported model wins over the bundled starter model, so a
     * relaunch keeps the model the user actually chose. The starter model is
     * copied out of the assets because hydra_engine_load() mmaps a real path
     * and assets live inside the APK zip.
     *
     * @return true when [modelFile] points at a usable .hydra.
     */
    private fun prepareModel(): Boolean {
        val imported = File(filesDir, IMPORTED_NAME)
        if (imported.isFile && imported.length() > HEADER_BYTES) {
            modelFile = imported
            return reportModel(imported, getString(R.string.log_model_imported))
        }

        val starter = File(filesDir, STARTER_NAME)
        try {
            if (!starter.isFile || starter.length() <= HEADER_BYTES) {
                assets.open(STARTER_ASSET).use { input ->
                    starter.outputStream().use { out ->
                        input.copyTo(out)
                        /* Sync before the loader may mmap it: a buffered
                         * write that has not reached storage shows up as a
                         * header the C loader cannot read. */
                        out.fd.sync()
                    }
                }
            }
        } catch (e: Throwable) {
            /* Never throw out of here: this runs inside onCreate, and an
             * exception there means no window at all - which is exactly the
             * "the app does nothing" symptom this function exists to remove. */
            log(getString(R.string.log_starter_failed, e.message ?: "unknown"))
            return false
        }

        if (!starter.isFile || starter.length() <= HEADER_BYTES) {
            log(getString(R.string.log_starter_failed, starter.length().toString()))
            return false
        }
        modelFile = starter
        return reportModel(starter, getString(R.string.log_model_starter))
    }

    /**
     * Log what is loaded and mirror it into the UI label.
     *
     * @param message a format template with one %s, e.g. the value of
     *   R.string.log_model_starter - a resolved template, not a resource id,
     *   because the caller picks between "starter" and "imported".
     */
    private fun reportModel(file: File, message: String): Boolean {
        val report = analyseHydraFile(file, file.length())
        if (!report.valid) {
            log(getString(R.string.log_model, file.name))
            log(getString(R.string.log_import_header, report.describe()))
            for (rule in report.rules.filter { !it.ok }) {
                log("  - ${rule.id}: ${rule.message}")
            }
            return false
        }
        log(String.format(java.util.Locale.ROOT, message, file.name))
        log(resources.getQuantityString(R.plurals.log_size, file.length().toInt(), file.length()))
        log(getString(R.string.log_import_header, report.describe()))
        modelLabel.text = buildString {
            append(getString(R.string.label_model))
            append(' ').append(file.name)
            append(" · dim ").append(report.fields["dim"])
            append(" · layers ").append(report.fields["layers"])
        }
        return true
    }

    private fun startInference() {
        if (running) return
        val startToken = tokenInput.text.toString().toIntOrNull() ?: 42
        val steps = (stepsInput.text.toString().toIntOrNull() ?: 32).coerceIn(1, 256)
        val file = modelFile ?: run {
            log(getString(R.string.err_no_model))
            modelLabel.setText(R.string.err_no_model)
            return
        }
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

                val target = File(filesDir, IMPORTED_NAME)
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
                if (reportModel(target, getString(R.string.log_model_imported))) {
                    log(getString(R.string.log_import_ok, target.name))
                    /* An import is an explicit "use this model now", so it
                     * runs straight away - otherwise the new model sits
                     * there until the next launch and looks like it was
                     * ignored. */
                    startInference()
                } else {
                    log(getString(R.string.log_import_rejected, getString(R.string.err_import_invalid)))
                }
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
        /** logcat tag for the app's own log lines, next to the JNI tag. */
        const val LOG_TAG = "Hydra"
        const val HEADER_BYTES = 24
        /** Asset shipped in the APK; a real, working .hydra, not a stub. */
        const val STARTER_ASSET = "starter.hydra"
        const val STARTER_NAME = "starter.hydra"
        const val IMPORTED_NAME = "imported.hydra"
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