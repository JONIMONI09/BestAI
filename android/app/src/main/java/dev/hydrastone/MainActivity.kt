package dev.hydrastone

import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.provider.OpenableColumns
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import dev.hydrastone.ui.HydraApp
import dev.hydrastone.ui.MainActions
import dev.hydrastone.ui.MainUiState
import dev.hydrastone.ui.Tab
import dev.hydrastone.ui.theme.HydraTheme
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.io.File

/**
 * Hydra-Stone Android front end (Jetpack Compose, Material 3).
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

    private var modelFile: File? = null

    /**
     * The engine the chat talks to.
     *
     * Both engines are constructed once and held here; the chat screen asks
     * for an [EngineInterface] and never learns which one it got. Selecting
     * llama.cpp is therefore a change to one line, not a change to the UI.
     */
    private val hydraEngine = HydraEngine()
    private val llamaEngine = LlamaEngine()

    private val engines: List<EngineInterface> = listOf(llamaEngine, hydraEngine)

    /**
     * llama.cpp is only packaged for arm64-v8a in this phase, so on the other
     * ABIs the selector must not offer an engine whose library is absent:
     * System.loadLibrary("llama_jni") would throw at class-initialisation and
     * take the whole app down, not just the engine.
     */
    private val availableEngines: List<EngineInterface> = buildList {
        if (Build.SUPPORTED_ABIS.contains("arm64-v8a")) add(llamaEngine)
        add(hydraEngine)
    }

    @Volatile
    private var selectedEngine: EngineInterface = hydraEngine

    /** Conversation state, kept outside composition so rotation cannot lose it. */
    private val chat = ChatStateHolder()

    private val _ui = MutableStateFlow(
        MainUiState(
            engines = engineInfos(),
            selectedEngineId = selectedEngine.id
        )
    )
    private val ui: StateFlow<MainUiState> = _ui.asStateFlow()

    /** Keeps the selector in sync with the engine that is actually loaded. */
    private fun engineInfos() = availableEngines.map {
        dev.hydrastone.ui.EngineInfo(it.id, it.displayName, it.isExperimental)
    }

    /** Run automatically on launch. Off = the app waits for Send. */
    @Volatile
    private var autoRun = true

    /** True while an inference thread is alive. Gates Send/Stop. */
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
            _ui.update { it.copy(log = lineCount.toString()) }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        /* Crash handler first: anything that goes wrong from here on is
         * caught and shown with a copy button. */
        CrashHandler.appContext = applicationContext
        CrashHandler.install()

        /* Compose owns the layout now.
         *
         * The programmatic-Views version had one ScrollView around a vertical
         * LinearLayout, because a bare LinearLayout on a 320x640 device lays
         * its remaining children out BELOW the bottom edge instead of making
         * them reachable - the token log went with them, so the app ran the
         * engine and showed nothing, which is indistinguishable from "it does
         * not run". The Scaffold below keeps the property that matters: every
         * control stays reachable, and the conversation keeps a guaranteed
         * minimum height so it is readable on a short screen. */
        setContent {
            HydraTheme {
                val state by ui.collectAsState()

                /* Runs once per composition, which is what root.post { … }
                 * did before. It reads ui.value rather than the captured
                 * `state`: prepareModel() runs after setContent returns, so
                 * the value from the first composition is stale and would
                 * see hasModel = false and silently never auto-run. */
                LaunchedEffect(Unit) {
                    if (autoRun && ui.value.hasModel) startInference()
                }

                HydraApp(
                    state = state,
                    actions = MainActions(
                        onTabSelected = { tab -> _ui.update { it.copy(selectedTab = tab) } },
                        onInputChanged = { text -> _ui.update { it.copy(input = text) } },
                        onSend = { sendMessage() },
                        onStop = {
                            chat.onCancelRequested()
                            syncChat()
                            log(getString(R.string.log_cancel_armed))
                            selectedEngine.cancel()
                        },
                        onNewConversation = { chat.clear(); syncChat() },
                        onImportModel = {
                            // */* rather than a MIME filter: .hydra has no
                            // reliable MIME mapping, and a filter would hide
                            // the file on most providers.
                            openModel.launch(arrayOf("*/*"))
                        },
                        onAutoRunChanged = { checked -> autoRun = checked },
                        onBenchmark = { runBenchmark() },
                        onEngineSelected = { id ->
                            availableEngines.firstOrNull { it.id == id }?.let { selectEngine(it) }
                        }
                    )
                )
            }
        }

        /* Model preparation FIRST, and guarded. An exception here used to
         * escape onCreate() and kill the activity before anything was drawn
         * - the app simply did not come up. */
        if (!prepareModel()) {
            log(getString(R.string.err_no_model))
            // The composer stays disabled and the reason is shown, instead of
            // a Send button that silently does nothing.
            _ui.update {
                it.copy(hasModel = false, modelLabel = getString(R.string.err_no_model))
            }
            return
        }

        /* Und dann laeuft es von selbst. */
        if (autoRun) {
            log(getString(R.string.log_auto_run))
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
     * "Wins" only means "is tried first". Existence and size are not
     * validity: a file that was interrupted mid-write, truncated by a full
     * filesystem, or replaced by something else entirely is still a file of
     * more than 24 bytes. So the imported file is **validated**, and an
     * invalid one falls through to the starter instead of returning false -
     * which used to leave the app with no model at all and the message
     * "No model available", for a model the user had never even asked to
     * remove. The warning is logged rather than swallowed.
     *
     * @return true when [modelFile] points at a usable .hydra.
     */
    private fun prepareModel(): Boolean {
        val imported = File(filesDir, IMPORTED_NAME)
        if (imported.isFile && imported.length() > HEADER_BYTES) {
            modelFile = imported
            if (reportModel(imported, getString(R.string.log_model_imported))) {
                return true
            }
            /* Invalid: fall through to the starter rather than giving up. The
             * file is NOT deleted - the user may be able to recover it, and a
             * silent delete of somebody's model is not ours to do. */
            log(getString(R.string.log_model_import_invalid, imported.name))
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
        _ui.update {
            it.copy(
                hasModel = true,
                selectedEngineId = selectedEngine.id,
                modelLabel = buildString {
                    append(file.name)
                    append(" · dim ").append(report.fields["dim"])
                    append(" · layers ").append(report.fields["layers"])
                }
            )
        }
        loadIntoSelectedEngine(file.absolutePath)
        return true
    }

    /** Loads the model into the currently selected engine. */
    private fun loadIntoSelectedEngine(path: String) {
        selectedEngine.load(path)
    }

    /** Sends whatever is in the composer. The engine, not the UI, is paged. */
    private fun sendMessage() {
        val text = ui.value.input
        if (text.isBlank()) return
        _ui.update { it.copy(input = "") }
        chat.onUserMessage(text)
        syncChat()
        startInference(text)
    }

    /**
     * Mirrors [ChatStateHolder] into the state the shell renders.
     *
     * Called after every chat mutation rather than collected in a coroutine:
     * the holder's streams are the source of truth and this is a plain copy,
     * so there is no second writer and nothing to keep in step in a lifecycle.
     * Safe to call from the engine's background thread - StateFlow.update is
     * atomic and the Compose runtime observes it from the main thread.
     */
    private fun syncChat() {
        _ui.update {
            it.copy(
                chatMessages = chat.messages.value,
                chatState = chat.state.value,
                lastTurnCancelled = chat.lastTurnCancelled.value
            )
        }
    }

    /**
     * Starts a run through [EngineInterface], never through a concrete engine.
     *
     * The steps default is the Settings token limit; the start token the old
     * single-screen UI took from a text field is gone, because a chat screen
     * has no field for it. The Hydra engine derives its token sequence from
     * the prompt itself.
     */
    private fun startInference(prompt: String = DEFAULT_PROMPT) {
        if (running) return
        val file = modelFile ?: run {
            log(getString(R.string.err_no_model))
            _ui.update { it.copy(hasModel = false, modelLabel = getString(R.string.err_no_model)) }
            return
        }
        lineCount.clear()
        _ui.update { it.copy(log = "") }
        setRunning(true)
        chat.onGenerationStarted()
        syncChat()
        log(resources.getQuantityString(R.plurals.log_running, MAX_STEPS, MAX_STEPS))

        val t0 = SystemClock.elapsedRealtime()
        Thread {
            selectedEngine.generate(
                prompt,
                MAX_STEPS,
                object : GenerationCallbacks {
                    override fun onFirstToken() {
                        chat.onFirstToken()
                        syncChat()
                    }

                    override fun onToken(tokenId: Int, text: String) {
                        // The chat state holder owns the accumulated message,
                        // so a token arriving during a rotation updates the
                        // model rather than a composable that is going away.
                        chat.onToken(tokenId, text)
                        syncChat()
                    }

                    override fun onComplete(summary: GenerationSummary) {
                        val wall = SystemClock.elapsedRealtime() - t0
                        if (summary.cancelled) {
                            // A cancelled run is reported as cancelled, never
                            // as a failure: the user asked for it, and the
                            // partial output stays on screen.
                            log(getString(R.string.log_cancelled))
                        } else if (summary.error != null) {
                            log(getString(R.string.log_error, summary.error))
                        } else {
                            log(getString(R.string.log_wall, wall))
                            log(getString(R.string.log_ok))
                        }
                        chat.onComplete(summary.cancelled)
                        syncChat()
                        setRunning(false)
                    }
                }
            )
        }.start()
    }

    /**
     * Send and Stop are mutually exclusive: the composer renders one button
     * that becomes Stop while a run is live, so there is never a second control
     * on screen that does nothing.
     */
    private fun setRunning(value: Boolean) {
        running = value
        _ui.update { it.copy(chatState = if (value) ChatState.GENERATING else ChatState.IDLE) }
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
        /* A benchmark and a generation run both drive the same native engine,
         * and the benchmark's numbers are only meaningful while nothing else is
         * competing for the same 4 compute threads. Two overlapping runs would
         * report timings for neither. Both entry points check this, and the
         * button is disabled while either is in flight, so the guard is
         * visible rather than a silent refusal. */
        if (running || ui.value.isBenchmarking) {
            log(getString(R.string.log_bench_busy))
            return
        }
        // The old screen took this from a text field. The chat screen has no
        // such field, so it uses the same bound the engine runs with.
        val steps = MAX_STEPS
        lineCount.clear()
        _ui.update { it.copy(isBenchmarking = true) }
        log(resources.getQuantityString(R.plurals.log_bench_start, steps, steps))

        Thread {
            try {
                val json = HydraBridge.benchmark(
                    file.absolutePath, steps,
                    object : HydraBridge.Callback {
                        override fun onTokens(tokens: IntArray, done: Boolean) = Unit
                        // Deliberately does nothing: the benchmark measures
                        // the CALLING CONVENTION, not the UI work. A real
                        // run updates the chat state per token, which is more
                        // expensive still - so these numbers are a floor.
                        override fun onToken(step: Int, token: Int) = Unit
                    }
                )
                log(getString(R.string.log_bench_result, json))
            } catch (e: Throwable) {
                log(getString(R.string.log_error, e.message))
            } finally {
                // In a finally, not after the log: an exception on the way out
                // would otherwise leave the button disabled forever.
                _ui.update { it.copy(isBenchmarking = false) }
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

        // Route on the extension, not on a guess. A GGUF goes to llama.cpp and
        // a .hydra to the v1 engine; running the v1 header rules over a GGUF
        // would reject it as "wrong magic" when it is in fact the right file
        // for a different engine.
        if (displayName.endsWith(".gguf", ignoreCase = true)) {
            importGguf(uri, displayName)
            return
        }

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
                /* The swap never destroys the previous model before the new
                 * one is in place, and puts it back if the swap fails. The old
                 * code deleted first and renamed second, so a failed rename
                 * left the user with no model at all - and the comment above it
                 * called that "atomic within the same directory", which is true
                 * of renameTo and false of delete-then-rename. See
                 * AtomicModelSwap for the full reasoning. */
                val swap = AtomicModelSwap.swap(tmp, target)
                if (swap !is AtomicModelSwap.Outcome.Swapped) {
                    tmp.delete()
                    val why = when (swap) {
                        is AtomicModelSwap.Outcome.NotSwapped -> swap.why
                        is AtomicModelSwap.Outcome.RolledBack -> swap.why
                        is AtomicModelSwap.Outcome.RestoreFailed -> swap.why
                        else -> "unknown"
                    }
                    log(getString(R.string.log_import_failed, why))
                    return@Thread
                }

                modelFile = target
                if (reportModel(target, getString(R.string.log_model_imported))) {
                    AtomicModelSwap.discardBackup(swap.backup)
                    log(getString(R.string.log_import_ok, target.name))
                    /* An import is an explicit "use this model now", so it
                     * runs straight away - otherwise the new model sits
                     * there until the next launch and looks like it was
                     * ignored. */
                    startInference()
                } else {
                    /* The file passed the header rules but is still not usable.
                     * Put the previous model back instead of leaving the app
                     * with a model it cannot load: the user chose "replace",
                     * and a failed replacement should end where it started. */
                    if (AtomicModelSwap.rollback(swap.backup, target)) {
                        modelFile = target
                        loadIntoSelectedEngine(target.absolutePath)
                        log(getString(R.string.log_import_restored, target.name))
                    } else {
                        log(getString(R.string.log_import_restore_failed, target.name))
                    }
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

    /**
     * Imports a GGUF model for llama.cpp.
     *
     * Copy-then-rename, exactly like the .hydra path, because llama.cpp mmaps
     * a real file path and a content:// URI is not one. The copy is validated
     * BEFORE the old model is replaced: a file that llama.cpp refuses must not
     * be able to leave the app with no model at all.
     */
    private fun importGguf(uri: Uri, displayName: String) {
        Thread {
            val tmp = File(filesDir, "import.gguf.tmp")
            try {
                var bytes = 0L
                contentResolver.openInputStream(uri).use { input ->
                    if (input == null) throw IllegalStateException(getString(R.string.err_no_stream))
                    tmp.outputStream().use { out ->
                        val buf = ByteArray(64 * 1024)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            bytes += n
                            if (bytes > MAX_GGUF_BYTES) {
                                throw IllegalStateException(
                                    getString(R.string.err_gguf_too_large_human, formatBytes(MAX_GGUF_BYTES))
                                )
                            }
                            out.write(buf, 0, n)
                        }
                        out.fd.sync()
                    }
                }

                // Cheap structural check before a multi-hundred-millisecond
                // native load: the first four bytes are the GGUF magic.
                val head = ByteArray(4)
                java.io.FileInputStream(tmp).use { input ->
                    if (input.read(head) != 4) {
                        tmp.delete()
                        log(getString(R.string.log_import_rejected, getString(R.string.err_gguf_truncated)))
                        return@Thread
                    }
                }
                val magic = (head[0].toLong() and 0xFF) or
                    ((head[1].toLong() and 0xFF) shl 8) or
                    ((head[2].toLong() and 0xFF) shl 16) or
                    ((head[3].toLong() and 0xFF) shl 24)
                if (magic != GGUF_MAGIC) {
                    tmp.delete()
                    log(
                        getString(
                            R.string.log_import_rejected,
                            getString(R.string.err_gguf_magic, hex32(magic))
                        )
                    )
                    return@Thread
                }

                val target = File(filesDir, IMPORTED_GGUF_NAME)
                /* Same swap as the .hydra path, for the same reason. The old
                 * comment here claimed "the load is attempted BEFORE the old
                 * model is dropped" while the code two lines above had already
                 * dropped it; the swap makes the comment true. */
                val swap = AtomicModelSwap.swap(tmp, target)
                if (swap !is AtomicModelSwap.Outcome.Swapped) {
                    tmp.delete()
                    val why = when (swap) {
                        is AtomicModelSwap.Outcome.NotSwapped -> swap.why
                        is AtomicModelSwap.Outcome.RolledBack -> swap.why
                        is AtomicModelSwap.Outcome.RestoreFailed -> swap.why
                        else -> "unknown"
                    }
                    log(getString(R.string.log_import_failed, why))
                    return@Thread
                }

                val result = llamaEngine.load(target.absolutePath)
                if (!result.ok) {
                    // A file llama.cpp refuses must leave the previous model in
                    // place rather than leaving the app with nothing.
                    log(getString(R.string.log_import_rejected, result.error ?: "load failed"))
                    if (AtomicModelSwap.rollback(swap.backup, target)) {
                        loadIntoSelectedEngine(target.absolutePath)
                        log(getString(R.string.log_import_restored, target.name))
                    } else {
                        log(getString(R.string.log_import_restore_failed, target.name))
                    }
                    return@Thread
                }
                AtomicModelSwap.discardBackup(swap.backup)

                selectedEngine = llamaEngine
                modelFile = target
                _ui.update {
                    it.copy(
                        hasModel = true,
                        selectedEngineId = llamaEngine.id,
                        modelLabel = describeGguf(result.detail)
                    )
                }
                chat.clear()
                syncChat()
                log(getString(R.string.log_import_ok, target.name))
                log(
                    getString(
                        R.string.log_gguf_loaded,
                        result.detail["architecture"] ?: "?",
                        result.detail["quant"] ?: "?",
                        result.detail["n_ctx"] ?: "?"
                    )
                )
            } catch (e: Throwable) {
                tmp.delete()
                log(getString(R.string.log_import_failed, e.message ?: "unknown"))
            }
        }.start()
    }

    /** Read-only metadata for the active GGUF. Nothing here is inferred. */
    private fun describeGguf(detail: Map<String, String>): String {
        val arch = detail["architecture"] ?: "unknown"
        val quant = detail["quant"] ?: "unknown"
        val ctx = detail["n_ctx"] ?: "?"
        val layers = detail["n_layer"] ?: "?"
        return "$arch · $quant · ctx $ctx · $layers layers"
    }

    private fun selectEngine(engine: EngineInterface) {
        if (engine === selectedEngine) return
        // Unload the other one FIRST: two models mapped at once on a phone is
        // how you get killed by lmkd rather than by your own code.
        if (engine !== llamaEngine) llamaEngine.unload()
        if (engine !== hydraEngine) hydraEngine.unload()

        val file = modelFile
        val result = if (file == null) {
            EngineLoadResult(false, getString(R.string.err_no_model))
        } else {
            engine.load(file.absolutePath)
        }

        selectedEngine = engine
        if (result.ok) {
            _ui.update {
                it.copy(
                    hasModel = true,
                    selectedEngineId = engine.id,
                    modelLabel = if (engine === llamaEngine) {
                        describeGguf(result.detail)
                    } else {
                        file?.name ?: ""
                    }
                )
            }
            chat.clear()
            syncChat()
        } else {
            // Say what happened instead of leaving a chat screen whose Send
            // button silently refuses to work.
            _ui.update {
                it.copy(
                    hasModel = false,
                    selectedEngineId = engine.id,
                    modelLabel = result.error ?: getString(R.string.err_engine_model_mismatch)
                )
            }
        }
        log(getString(R.string.log_engine_selected, engine.displayName))
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

        /**
         * Token budget for one run. The native side accepts 1..256 steps and
         * rejects anything below 1 rather than clamping, so the bound is the
         * same one the engine advertises.
         */
        const val MAX_STEPS = 32

        /** Prompt used by the automatic run on launch, which has no input yet. */
        const val DEFAULT_PROMPT = "hydra demo"
        /** logcat tag for the app's own log lines, next to the JNI tag. */
        const val LOG_TAG = "Hydra"
        const val HEADER_BYTES = 24
        /** Asset shipped in the APK; a real, working .hydra, not a stub. */
        const val STARTER_ASSET = "starter.hydra"
        const val STARTER_NAME = "starter.hydra"
        const val IMPORTED_NAME = "imported.hydra"
        const val IMPORTED_GGUF_NAME = "imported.gguf"

        /**
         * Upper bound on a GGUF import.
         *
         * Deliberately generous: a phone cannot run a 30 GB model anyway, but
         * refusing a file because of a guessed ceiling would be worse than
         * letting llama.cpp load it and report what it actually needed. The
         * real limit is memory, and llama.cpp's own loader is what finds it.
         */
        const val MAX_GGUF_BYTES = 4L * 1024 * 1024 * 1024
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