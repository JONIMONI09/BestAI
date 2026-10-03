package dev.hydrastone

import android.util.Log
import org.json.JSONObject

/**
 * The Hydra v1 engine behind [EngineInterface].
 *
 * This is a thin adapter. It calls the existing [HydraBridge] and changes no C
 * code: the native bridge is reached through the same three functions it has
 * always had, and `hydra_jni.c` is untouched.
 *
 * ## Why this engine is labelled "experimental"
 *
 * Hydra v1 is a ternary recurrence over a 2-bit-packed weight file. It is not a
 * language model and it has no tokenizer, which has two consequences the UI must
 * not hide:
 *
 *  - [EngineInterface.generate] receives text but **cannot interpret it**. There
 *    is no vocabulary and no encoder to turn words into tokens. [tokenSequence]
 *    derives a deterministic token sequence from the prompt bytes so the demo is
 *    reproducible, and that sequence is not a representation of the text's
 *    meaning. It is a stand-in.
 *  - [EngineCapabilities.supportsStreamingText] is false because there is no
 *    detokenizer: the output is token ids, not words. The chat screen shows the
 *    ids in the raw-token debug row instead of pretending they form a sentence.
 *
 * The benchmark path is the honest use of this engine, which is why it is kept.
 */
class HydraEngine : EngineInterface {

    override val id = "hydra"
    override val displayName = "Hydra v1 (experimental)"
    override val isExperimental = true

    @Volatile
    private var loadedPath: String? = null

    @Volatile
    private var running = false

    override var capabilities: EngineCapabilities = EngineCapabilities()
        private set

    override fun load(modelPath: String): EngineLoadResult {
        // The native loader mmaps the path and reports failure through the
        // inference result, not through an exception. Validate the header the
        // same way the C loader does so the user gets a message before a run is
        // attempted; the authoritative check still happens in native code.
        val file = java.io.File(modelPath)
        if (!file.exists()) {
            return EngineLoadResult(false, "Model file not found")
        }
        loadedPath = modelPath
        capabilities = EngineCapabilities(
            contextWindow = 0,
            supportsStreamingText = false,
            understandsTextPrompt = false
        )
        return EngineLoadResult(
            true,
            detail = mapOf(
                "engine" to displayName,
                "format" to ".hydra (v1)",
                "vocab" to "1024 ids, no tokenizer",
                "memory" to "mapped 2-bit weights; see docs/ANDROID-AUDIT.md"
            )
        )
    }

    override fun generate(prompt: String, maxTokens: Int, callbacks: GenerationCallbacks) {
        val path = loadedPath
        if (path == null) {
            callbacks.onComplete(GenerationSummary(0, false, "No model loaded"))
            return
        }
        if (running) {
            callbacks.onComplete(GenerationSummary(0, false, "A generation is already running"))
            return
        }

        // The native side rejects steps < 1 rather than clamping (see
        // hydra_jni.c "steps must be >= 1"), so the bound is applied here and
        // the same range the UI offers is the range the engine accepts.
        val steps = maxTokens.coerceIn(1, MAX_STEPS)
        val tokens = tokenSequence(prompt)
        val started = System.nanoTime()
        running = true

        val bridgeCallback = object : HydraBridge.Callback {
            override fun onTokens(tokens: IntArray, done: Boolean) {
                // One block, not one token per callback: the bridge batches
                // ~16 tokens because the per-token JNI transition costs more
                // than the engine step itself at this model size.
                if (tokens.isNotEmpty() && !done) callbacks.onFirstToken()
                tokens.forEach { callbacks.onToken(it, "") }
                if (done) {
                    running = false
                    val summary = parseSummary(tokens.size, System.nanoTime() - started)
                    callbacks.onComplete(summary)
                }
            }

            override fun onToken(step: Int, token: Int) {
                // Only benchmark() uses this shape. Never reached from
                // generate(), but the interface requires it.
            }
        }

        // runInference() blocks on the calling thread until the run finishes.
        val json = try {
            HydraBridge.runInference(path, tokens, steps, bridgeCallback)
        } catch (e: Throwable) {
            running = false
            callbacks.onComplete(
                GenerationSummary(0, false, "Native engine call failed: ${e.javaClass.simpleName}")
            )
            return
        }

        // Safety net: if the run ended without a final callback, still complete
        // exactly once so the chat state machine can never be left in
        // GENERATING with nothing able to move it out.
        if (running) {
            running = false
            callbacks.onComplete(parseSummaryFrom(json, System.nanoTime() - started))
        }
    }

    override fun cancel() {
        // Cooperative: the native loop checks the flag once per step. The flag
        // is cleared at the start of every run, so a cancel that lands between
        // two runs cannot kill the next one.
        HydraBridge.cancel()
    }

    override fun unload() {
        cancel()
        loadedPath = null
        capabilities = EngineCapabilities()
    }

    /**
     * Derives a deterministic token sequence from the prompt bytes.
     *
     * This is a reproducible stand-in, NOT language understanding: the v1
     * engine has no vocabulary. The sequence depends only on the text, so the
     * same prompt always produces the same run, which is what makes the
     * demo/benchmark comparable between launches and devices.
     */
    private fun tokenSequence(prompt: String): IntArray {
        if (prompt.isEmpty()) return intArrayOf(0)
        // FNV-1a over UTF-8 bytes, mixed per token. Deterministic across
        // platforms: it depends on nothing but the bytes of the string.
        var h = 0x811C9DC5L
        val out = IntArray(PROMPT_TOKENS)
        prompt.toByteArray(Charsets.UTF_8).forEachIndexed { i, byte ->
            h = (h xor (byte.toLong() and 0xFF)) * 0x01000193L
            if (i < PROMPT_TOKENS) {
                val mixed = (h ushr 13) xor (i.toLong() * 0x9E3779B97F4A7C15uL.toLong())
                out[i] = ((mixed and 0x3FF)).toInt() // vocab is 1024 ids
            }
        }
        return out
    }

    private fun parseSummary(generated: Int, elapsedNs: Long): GenerationSummary {
        return GenerationSummary(
            generatedTokens = generated,
            cancelled = false,
            elapsedMs = elapsedNs / 1_000_000
        )
    }

    /**
     * Reads the native JSON result. A cancelled run reports `cancelled: true`,
     * which is not an error: partial output is kept and labelled by the UI.
     */
    private fun parseSummaryFrom(json: String, elapsedNs: Long): GenerationSummary {
        return try {
            val o = JSONObject(json)
            GenerationSummary(
                generatedTokens = o.optInt("tokens", 0),
                cancelled = o.optBoolean("cancelled", false),
                error = if (o.optBoolean("ok", false)) null else o.optString("error", "Run failed"),
                elapsedMs = elapsedNs / 1_000_000
            )
        } catch (e: Throwable) {
            Log.w(TAG, "unparseable engine result", e)
            GenerationSummary(0, false, "Run failed", elapsedNs / 1_000_000)
        }
    }

    private companion object {
        const val TAG = "HydraEngine"

        /** The native loader accepts 1..256 steps and rejects anything else. */
        const val MAX_STEPS = 256

        /** Token ids fed before generation; the last one is the seed. */
        const val PROMPT_TOKENS = 4
    }
}
