package dev.hydrastone

/** JNI bridge to the Hydra-Stone C engine. */
object HydraBridge {
    interface Callback {
        /** Called on the calling thread for every generated token. */
        fun onToken(step: Int, token: Int)
    }

    init {
        System.loadLibrary("hydra")
    }

    external fun runInference(
        modelPath: String,
        startToken: Int,
        steps: Int,
        callback: Callback
    ): String
}
