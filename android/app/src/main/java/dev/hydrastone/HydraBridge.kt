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

        /**
         * The old per-token shape. Not used by runInference() any more;
         * only benchmark() calls it, so the delta can be measured instead
         * of assumed.
         */
        fun onToken(step: Int, token: Int)
    }

    init {
        System.loadLibrary("hydra")
    }

    /**
     * @param modelPath absolute path of a .hydra file on local storage
     * @param prompt    token ids fed through the engine before generation;
     *                  the last entry is the seed. Empty = start at token 0.
     * @param steps     number of generated steps (1..256). A value below 1
     *                  is rejected by the native side - not clamped - and
     *                  answers {"ok":false,"error":"steps must be >= 1"}.
     * @return a JSON summary string, never null. A cancelled run answers
     *         {"ok":false,"cancelled":true,...}, which is not an error.
     */
    external fun runInference(
        modelPath: String,
        prompt: IntArray,
        steps: Int,
        callback: Callback
    ): String

    /**
     * Asks the run in progress to stop.
     *
     * This is a real, cooperative cancel, not a UI detach: the native loop
     * checks the flag once per step, returns within one step, and reports
     * `cancelled: true`. It is called from the UI thread and returns
     * immediately; it does not wait for the run to finish.
     *
     * The flag is cleared at the start of every run, so a cancel that
     * arrives between two runs cannot kill the next one.
     */
    external fun cancel()

    /**
     * Phase 0 measurement on the real device. Runs the same model three
     * times and returns JSON with ns/token for:
     *  - the native loop alone (no JNI),
     *  - a JNI callback per token,
     *  - the shipping batched callback (16 tokens per call).
     *
     * [callback] must also implement onToken(step, token): the benchmark
     * calls it deliberately, to measure the cost of the old per-token
     * shape against the new batched one.
     */
    external fun benchmark(
        modelPath: String,
        steps: Int,
        callback: Callback
    ): String
}