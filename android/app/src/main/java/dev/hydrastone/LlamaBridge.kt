package dev.hydrastone

/**
 * JNI bridge to llama.cpp.
 *
 * Deliberately separate from [HydraBridge]: the two engines load different
 * shared objects (`libllama_jni.so` and `libhydra.so`) and share no state. A
 * fault while loading one library cannot stop the other from loading, and the
 * v1 bridge stays exactly as it was.
 *
 * Every method returns a JSON string rather than throwing, because every
 * failure a user can cause - a corrupt file, an unsupported architecture, a
 * context too large for the phone - is an ordinary outcome that needs a
 * readable sentence, not an exception (rules.md R27).
 */
object LlamaBridge {

    /**
     * Streaming callbacks for one generation.
     *
     * Called on the native calling thread, which is the background thread the
     * generation was started on. Never the main thread.
     */
    interface Callback {
        /** Exactly once, before the first piece of text. */
        fun onFirstToken()

        /** Per token. [piece] is the detokenized text; [tokenId] is the raw id. */
        fun onPiece(piece: String, tokenId: String)

        /** Exactly once, after the last onPiece and before generate() returns. */
        fun onDone()
    }

    init {
        System.loadLibrary("llama_jni")
    }

    /**
     * Loads a GGUF model and creates an inference context.
     *
     * @param nCtx context window in tokens. The context the library actually
     *             creates can differ from this, so the reply reports the real
     *             value rather than the requested one.
     * @return JSON with ok, architecture, n_ctx, n_ctx_train, n_embd, n_layer,
     *         size_bytes, state_bytes and quant_type, or ok:false with an
     *         error the user can act on.
     */
    external fun load(path: String, nCtx: Int): String

    /**
     * Generates from [prompt], streaming to [callback]. Blocks until done.
     *
     * @return JSON with ok, generated, cancelled, truncated, n_past and n_ctx.
     *         `cancelled` is not an error: it means the user pressed Stop.
     */
    external fun generate(
        prompt: String,
        maxTokens: Int,
        temperature: Float,
        callback: Callback
    ): String

    /**
     * Asks the running generation to stop.
     *
     * Cooperative: the decode loop checks an atomic flag once per token, so
     * the run ends within one token and the partial text is kept. Returns
     * immediately and does not wait.
     */
    external fun cancel()

    /** Frees the model and its context. Safe to call when nothing is loaded. */
    external fun unload()
}
