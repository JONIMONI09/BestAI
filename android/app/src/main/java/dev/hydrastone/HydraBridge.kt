package dev.hydrastone

/** JNI bridge to the Hydra-Stone C engine. */
object HydraBridge {
    /**
     * Progress callback.
     *
     * Delivered in blocks of ~16 tokens instead of once per token: the
     * per-token JNI transition (native -> Kotlin object method, plus the
     * UI update behind it) costs more than the engine step itself at this
     * model size. [tokens] is the block of newly generated tokens;
     * [done] is true exactly once, on the final block.
     */
    interface Callback {
        /** Called on the calling (background) thread. */
        fun onTokens(tokens: IntArray, done: Boolean)
    }

    init {
        System.loadLibrary("hydra")
    }

    /**
     * @param modelPath absolute path of a .hydra file on local storage
     * @param prompt    token ids fed through the engine before generation;
     *                  the last entry is the seed. Empty = start at token 0.
     * @param steps     number of generated steps (clamped to 1..256)
     * @return a JSON summary string, never null
     */
    external fun runInference(
        modelPath: String,
        prompt: IntArray,
        steps: Int,
        callback: Callback
    ): String
}