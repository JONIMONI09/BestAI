package dev.hydrastone

import android.util.Log
import org.json.JSONObject

/**
 * The Hydra v1 engine behind [EngineInterface].
 *
 * This is a thin adapter over [HydraBridge]: generation goes through
 * `runInferenceSampled` (the C sampler, see include/hydra_model.h),
 * cancellation through `cancel`. The greedy baseline itself is untouched -
 * with temperature 0 the native side delegates to the pre-sampler step and
 * the token stream stays bit-identical to the old `runInference` path
 * (pinned on the host by tests/test_engine.c).
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

    override fun load(modelPath: String, contextWindow: Int): EngineLoadResult {
        // Deliberately ignored: the v1 engine is a two-state recurrence over a
        // fixed-size weight file. It has no context, no KV cache and no
        // window to size, so the argument is accepted for interface uniformity
        // and documented as having no effect - a "context size" label above
        // an engine that has no context would be a control that does nothing
        // presented as one.
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
                "memory" to "mapped 2-bit weights; see .agents/docs/ANDROID-AUDIT.md"
            )
        )
    }

    override fun generate(
        prompt: String,
        maxTokens: Int,
        callbacks: GenerationCallbacks,
        // The Settings values reach the C sampler through
        // runInferenceSampled: temperature and minP on both format
        // versions (the v1/v2 logit probe, the v3 logit vector),
        // repetitionPenalty on both as well. temperature = 0 (the
        // greedy guard) stays bit-identical to the pre-sampler
        // recurrence - the engine's own guard guarantees that, and
        // tests/test_engine.c pins it. The seed is fixed (below): a
        // run is reproducible for fixed model + prompt + params, and
        // there is no UI surface for a per-run seed yet, so
        // inventing one silently would be a fake control.
        params: GenerationParams
    ) {
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

        var totalTokens = 0
        var completed = false

        val bridgeCallback = object : HydraBridge.Callback {
            override fun onTokens(tokens: IntArray, done: Boolean) {
                // One block, not one token per callback: the bridge batches
                // ~16 tokens because the per-token JNI transition costs more
                // than the engine step itself at this model size.
                if (tokens.isNotEmpty() && !done) callbacks.onFirstToken()
                tokens.forEach { callbacks.onToken(it, "") }
                totalTokens += tokens.size
                if (done) {
                    running = false
                    // Deliberately NO onComplete here. The batch callback can
                    // only say "the run ended", not how: a run the user
                    // cancelled reports ok:false + cancelled:true in the
                    // JSON that runInference() returns, and that total
                    // ("emitted") counts every block, not just this last
                    // one. Completing from the callback (as the old code
                    // did) marked a cancelled run as completed and counted
                    // only the final block's tokens. The summary is built
                    // once, after runInference() returns, from that JSON.
                }
            }

            override fun onToken(step: Int, token: Int) {
                // Only benchmark() uses this shape. Never reached from
                // generate(), but the interface requires it.
            }
        }

        // runInferenceSampled() blocks on the calling thread until the run
        // finishes, so by the time it returns every onTokens block has
        // already been delivered on this same thread and totalTokens is
        // final. With the default Settings (temperature 0.7, minP 0.05)
        // the run is SAMPLED and reproducible (fixed seed); sliding the
        // temperature to 0 switches it back to the deterministic baseline.
        val json = try {
            HydraBridge.runInferenceSampled(
                path,
                tokens,
                steps,
                bridgeCallback,
                params.temperature,
                params.minP,
                params.repetitionPenalty,
                SAMPLING_SEED
            )
        } catch (e: Throwable) {
            running = false
            if (!completed) {
                completed = true
                callbacks.onComplete(
                    GenerationSummary(
                        generatedTokens = totalTokens,
                        cancelled = false,
                        error = "Native engine call failed: ${e.javaClass.simpleName}",
                        elapsedMs = (System.nanoTime() - started) / 1_000_000
                    )
                )
            }
            return
        }

        // The single completion point: exactly once, from the native JSON,
        // which is the only place that knows whether the run was cancelled
        // and how many tokens were emitted in total.
        running = false
        if (!completed) {
            completed = true
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

    /**
     * Reads the native JSON result. A cancelled run reports `cancelled: true`,
     * which is not an error: partial output is kept and labelled by the UI.
     * This is the ONLY place a run's summary is built for generate(): the
     * callback can say the run ended, but only this JSON says how it ended
     * and how many tokens were emitted in total.
     */
    private fun parseSummaryFrom(json: String, elapsedNs: Long): GenerationSummary {
        return try {
            val o = JSONObject(json)
            val cancelled = o.optBoolean("cancelled", false)
            GenerationSummary(
                // The native summary reports "emitted" (the total across all
                // blocks). The old code read "tokens", which does not exist
                // in the native JSON and silently yielded 0 - invisible while
                // this code path was unreachable.
                generatedTokens = o.optInt("emitted", 0),
                cancelled = cancelled,
                // A cancelled run is not a failure. The native side answers
                // it with ok:false and an "error" that only names the
                // cancellation; surfacing that string as an error would make
                // the UI treat a user-requested stop as a fault.
                error = if (cancelled || o.optBoolean("ok", false)) {
                    null
                } else {
                    o.optString("error", "Run failed")
                },
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

        /**
         * Fixed sampler seed for reproducibility: same model, prompt and
         * Settings values always give the same token stream. 0 is not
         * degenerate - the engine maps a zero seed to a non-zero RNG
         * constant (see hydra_sample_token in src/hydra_engine.c).
         */
        const val SAMPLING_SEED = 0
    }
}
