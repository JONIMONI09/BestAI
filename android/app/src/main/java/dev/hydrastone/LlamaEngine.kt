package dev.hydrastone

import android.util.Log
import org.json.JSONObject

/**
 * The real inference engine: llama.cpp, pinned at v0.5.0
 * (7fe450e19305b828c199d602c23a8337aaa1f03b), MIT licensed.
 *
 * This is the engine the app is actually for. [HydraEngine] stays as the
 * secondary token demo; neither knows about the other, and the chat screen
 * cannot tell them apart except through [isExperimental].
 *
 * ## What this class deliberately does not do
 *
 * It does not decide whether a GGUF file is supported. llama.cpp's own loader
 * is the authority on that: if it accepts the file, the architecture and
 * quantisation are ones this build understands. The Models tab shows the
 * support matrix and refuses to claim more than the table says, but the
 * decision here is delegated rather than reimplemented, because a second
 * implementation of "which architectures work" would drift from the first.
 */
class LlamaEngine : EngineInterface {

    override val id = "llama"
    override val displayName = "llama.cpp"
    override val isExperimental = false

    @Volatile
    private var loadedPath: String? = null

    @Volatile
    private var running = false

    /** Metadata from the last successful load, for the Models tab. */
    @Volatile
    var modelInfo: Map<String, String> = emptyMap()
        private set

    override var capabilities: EngineCapabilities = EngineCapabilities()
        private set

    override fun load(modelPath: String, contextWindow: Int): EngineLoadResult {
        if (!java.io.File(modelPath).isFile) {
            return EngineLoadResult(false, "Model file not found")
        }

        // Zero means "the engine's default". The UI's control lives in
        // 256..8192; a request outside that range is clamped rather than
        // passed through, because the KV cache is allocated per context
        // token (2 * n_layer * n_ctx * n_embd * 2 bytes at this pin's F16
        // KV default) and 32k tokens on a small model is how phones get
        // killed by lmkd. The actually granted value is read back below.
        val requested = if (contextWindow <= 0) DEFAULT_CONTEXT
        else contextWindow.coerceIn(MIN_CONTEXT, MAX_CONTEXT)

        // llama.cpp maps the file directly, so this must be a real path. The
        // SAF import copies for exactly this reason.
        val json = try {
            LlamaBridge.load(modelPath, requested)
        } catch (e: Throwable) {
            Log.e(TAG, "native load failed", e)
            return EngineLoadResult(
                false,
                "The native llama.cpp library could not be called on this device."
            )
        }

        val obj = parse(json) ?: return EngineLoadResult(false, "The model could not be read")
        if (!obj.optBoolean("ok", false)) {
            return EngineLoadResult(false, obj.optString("error", "The model could not be loaded"))
        }

        loadedPath = modelPath
        val nCtx = obj.optInt("n_ctx", DEFAULT_CONTEXT)
        capabilities = EngineCapabilities(
            contextWindow = nCtx,
            supportsStreamingText = true,
            understandsTextPrompt = true
        )

        modelInfo = buildMap {
            put("architecture", obj.optString("architecture", "unknown"))
            put("quant", obj.optString("quant_type", "unknown"))
            put("n_ctx", nCtx.toString())
            put("n_ctx_train", obj.optInt("n_ctx_train", 0).toString())
            put("n_embd", obj.optInt("n_embd", 0).toString())
            put("n_layer", obj.optInt("n_layer", 0).toString())
            put("size_bytes", obj.optLong("size_bytes", 0L).toString())
            // The KV cache the context allocates, from the bridge: 2 *
            // n_layer * n_ctx * n_embd * 2 bytes (F16 K and V at this pin).
            // This is the RAM figure the header shows, not an estimate.
            put("kv_bytes", obj.optLong("kv_bytes", 0L).toString())
            // One f32 per vocabulary entry, per output position.
            put("logits_bytes", obj.optLong("logits_bytes", 0L).toString())
            // The serialized state at load (n_past = 0): sampler/RNG
            // metadata, a few bytes. Kept because it is llama.cpp's own
            // number; the header labels it accordingly.
            put("state_bytes", obj.optLong("state_bytes", 0L).toString())
        }

        return EngineLoadResult(true, detail = modelInfo)
    }

    override fun generate(
        prompt: String,
        maxTokens: Int,
        callbacks: GenerationCallbacks,
        params: GenerationParams
    ) {
        if (loadedPath == null) {
            callbacks.onComplete(GenerationSummary(0, false, "No model loaded"))
            return
        }
        if (running) {
            callbacks.onComplete(GenerationSummary(0, false, "A generation is already running"))
            return
        }

        running = true
        val started = System.nanoTime()

        val bridgeCallback = object : LlamaBridge.Callback {
            override fun onFirstToken() = callbacks.onFirstToken()

            override fun onPiece(piece: String, tokenId: String) {
                callbacks.onToken(tokenId.toIntOrNull() ?: -1, piece)
            }

            override fun onDone() = Unit
        }

        // The Settings values pass through untouched on purpose: the native
        // side clamps with the same bounds and logs every clamp, so a number
        // that cannot run is reported rather than silently altered here.
        val json = try {
            LlamaBridge.generate(
                prompt,
                maxTokens,
                params.temperature,
                params.topK,
                params.topP,
                params.minP,
                bridgeCallback
            )
        } catch (e: Throwable) {
            running = false
            callbacks.onComplete(
                GenerationSummary(0, false, "Native generation failed: ${e.javaClass.simpleName}")
            )
            return
        }

        running = false
        val obj = parse(json)
        val elapsedMs = (System.nanoTime() - started) / 1_000_000

        if (obj == null) {
            callbacks.onComplete(GenerationSummary(0, false, "The result could not be read", elapsedMs))
            return
        }

        val truncated = obj.optBoolean("truncated", false)
        val error = if (truncated) {
            // Not a failure: the conversation simply outgrew the context. The
            // oldest tokens were dropped and the user is told so.
            "The conversation was longer than the model's context window; the oldest part was dropped."
        } else if (!obj.optBoolean("ok", false)) {
            obj.optString("error", "Generation failed")
        } else {
            null
        }

        callbacks.onComplete(
            GenerationSummary(
                generatedTokens = obj.optInt("generated", 0),
                cancelled = obj.optBoolean("cancelled", false),
                error = error,
                elapsedMs = elapsedMs
            )
        )
    }

    override fun cancel() {
        // Sets an atomic flag the decode loop reads once per token. Does not
        // block and does not unwind the matmul, so the text produced so far
        // stays on screen.
        LlamaBridge.cancel()
    }

    override fun unload() {
        cancel()
        LlamaBridge.unload()
        loadedPath = null
        modelInfo = emptyMap()
        capabilities = EngineCapabilities()
    }

    private fun parse(json: String): JSONObject? = try {
        JSONObject(json)
    } catch (e: Throwable) {
        Log.w(TAG, "unparseable llama.cpp result", e)
        null
    }

    private companion object {
        const val TAG = "LlamaEngine"

        /**
         * Context window used when no model is active.
         *
         * Small on purpose: the KV cache is allocated per context token, and a
         * phone asked for 32k tokens on a small model will fail to allocate.
         * The value actually granted is read back from llama_n_ctx() and
         * reported, never assumed.
         */
        const val DEFAULT_CONTEXT = 2048

        /** Bounds of the Settings context control, see [load]. */
        const val MIN_CONTEXT = 256
        const val MAX_CONTEXT = 8192
    }
}
