package dev.hydrastone

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/**
 * Proves the llama.cpp engine works on a real Android runtime.
 *
 * WHY AN INSTRUMENTED TEST AND NOT A HOST TEST
 * --------------------------------------------
 * The engine under test is `libllama_jni.so`, linked into this APK and reachable
 * only through JNI from inside the app process. A host-side test could compile
 * against the headers and still prove nothing about whether the shipped library
 * loads a model, emits tokens, or stops when cancelled (rules.md R23, R32).
 * So this runs where the library actually lives.
 *
 * WHAT IT ASSERTS, AND WHAT IT DELIBERATELY DOES NOT
 * -------------------------------------------------
 * It asserts BEHAVIOUR that the user can observe: a model loads, tokens arrive
 * one at a time in order, and pressing Stop ends the run while keeping what was
 * already produced. It asserts NOTHING about speed. The emulator this runs on
 * is a software-emulated (TCG) x86_64 guest, so any tok/s measured here would be
 * a property of the emulator and not of any phone. Timing is therefore
 * deliberately absent: this file contains no throughput assertion and no
 * benchmark.
 *
 * THE MODEL
 * ---------
 * Supplied by the host, never committed. The path comes from the
 * `hydraModelPath` instrumentation argument; without it every test in this
 * class is SKIPPED (assumeTrue), never silently passed. A skipped test must
 * never be reported as a green run.
 */
@RunWith(AndroidJUnit4::class)
class LlamaEngineInstrumentedTest {

    private companion object {
        const val TAG = "HydraLlamaTest"

        /** 512 keeps the KV cache small enough for a 2 GiB software-emulated guest. */
        const val N_CTX = 512

        /**
         * The app's own external files directory needs no runtime permission on
         * any API level, which is why the model is pushed there rather than to
         * /sdcard/Download (that one is permission-gated from API 23).
         */
        const val DEFAULT_MODEL_PATH =
            "/sdcard/Android/data/dev.hydrastone/files/smollm2-135m-q4_k_m.gguf"

        /**
         * How long a prefill-time cancel is given to unwind.
         *
         * Stop is cooperative and is read BETWEEN prefill batches, so a cancel
         * issued during prompt evaluation cannot take effect until the batch in
         * flight returns. On this TCG guest a batch takes minutes; on a phone
         * it takes a fraction of a second. This number is therefore a property
         * of the emulator, not of the product, and it is deliberately loose: it
         * exists so a real hang is still caught, not to measure anything.
         */
        const val PREFILL_CANCEL_GRACE_MS = 900_000L
    }

    private val modelPath: String =
        InstrumentationRegistry.getArguments().getString("hydraModelPath")
            ?: DEFAULT_MODEL_PATH

    private var loaded = false

    // ---- one collector, so both the streaming and the cancel test can see the
    // ---- same three callbacks in the order the bridge promises.
    private class Collector : LlamaBridge.Callback {
        val firstToken = AtomicBoolean(false)
        val pieces = mutableListOf<String>()
        val ids = mutableListOf<String>()
        val done = AtomicBoolean(false)
        /** Set when a piece arrives before onFirstToken, i.e. a contract violation. */
        val outOfOrder = AtomicBoolean(false)
        private val arrived = CountDownLatch(1)

        override fun onFirstToken() {
            firstToken.set(true)
        }

        override fun onPiece(piece: String, tokenId: String) {
            if (!firstToken.get()) outOfOrder.set(true)
            synchronized(pieces) {
                pieces.add(piece)
                ids.add(tokenId)
            }
            arrived.countDown()
        }

        override fun onDone() {
            done.set(true)
        }

        fun awaitFirstPiece(seconds: Long): Boolean = arrived.await(seconds, TimeUnit.SECONDS)
    }

    @Before
    fun setUp() {
        val f = File(modelPath)
        assumeTrue(
            "no GGUF at $modelPath - push one with tools/fetch_test_model.sh and pass " +
                "-e hydraModelPath <path>. This run is SKIPPED, not passed.",
            f.isFile && f.length() > 0
        )
        Log.i(TAG, "using model ${f.absolutePath} (${f.length()} bytes)")
    }

    @After
    fun tearDown() {
        // Always release the mapping, even after a failed assertion: the next
        // test would otherwise try to load a second copy of the same weights
        // into a guest with 2 GiB of RAM.
        LlamaBridge.unload()
        loaded = false
    }

    @Test
    fun loadReportsRealModelMetadata() {
        val fileBytes = File(modelPath).length()
        val meta = JSONObject(LlamaBridge.load(modelPath, N_CTX))
        assertTrue("load failed: $meta", meta.getBoolean("ok"))
        loaded = true

        // The reply must describe the file that was actually opened, not echo
        // back the arguments. SmolLM2-135M is a Llama-family model with 576
        // hidden dimensions and 30 layers; if these are wrong the loader is
        // reporting something it did not read.
        val arch = meta.getString("architecture")
        assertTrue("unexpected architecture '$arch'", arch.contains("llama"))
        assertEquals(576, meta.getInt("n_embd"))
        assertEquals(30, meta.getInt("n_layer"))
        assertEquals(N_CTX, meta.getInt("n_ctx"))
        assertTrue("quant type must be reported", meta.getString("quant_type").isNotBlank())
        assertTrue("state bytes must be positive", meta.getLong("state_bytes") > 0)

        // `size_bytes` is llama.cpp's llama_model_size(): the WEIGHTS, i.e. the
        // sum of the tensor byte ranges. It is deliberately NOT the size of the
        // file, which additionally carries the GGUF header, the KV metadata and
        // the tensor directory. For this model the two differ by about 1.7 MB.
        // Asserting the relationship rather than a magic constant is the point:
        // it catches a loader that reports the file size it was handed, and it
        // catches one that reports nothing sensible, without pretending the
        // metadata overhead is zero.
        val sizeBytes = meta.getLong("size_bytes")
        assertTrue("size_bytes $sizeBytes must be positive", sizeBytes > 0)
        assertTrue(
            "size_bytes $sizeBytes should be below the file size $fileBytes",
            sizeBytes < fileBytes
        )
        val overhead = fileBytes - sizeBytes
        assertTrue(
            "GGUF overhead $overhead is implausibly large for a $fileBytes byte file",
            overhead * 20 < fileBytes
        )

        Log.i(
            TAG,
            "loaded arch=$arch n_embd=${meta.getInt("n_embd")} n_layer=${meta.getInt("n_layer")} " +
                "n_ctx=${meta.getInt("n_ctx")} n_ctx_train=${meta.getInt("n_ctx_train")} " +
                "quant=${meta.getString("quant_type")} " +
                "weights=$sizeBytes file=$fileBytes gguf_overhead=$overhead " +
                "state_bytes=${meta.getLong("state_bytes")}"
        )
    }

    @Test
    fun generatesAndStreamsTokensInOrder() {
        loadOrFail()

        val c = Collector()
        val raw = LlamaBridge.generate("Hello", 8, 0.0f, c)

        assertTrue("generate failed: $raw", JSONObject(raw).getBoolean("ok"))
        assertTrue("onFirstToken never fired", c.firstToken.get())
        assertTrue("a piece arrived before onFirstToken", !c.outOfOrder.get())
        assertTrue("onDone never fired", c.done.get())
        assertTrue("no token was streamed", c.pieces.isNotEmpty())
        assertEquals("raw ids and pieces must be the same length", c.pieces.size, c.ids.size)
        c.ids.forEach { assertTrue("token id '$it' is not an integer", it.toIntOrNull() != null) }

        val text = c.pieces.joinToString("")
        assertTrue("streamed text was blank", text.isNotBlank())
        Log.i(TAG, "streamed ${c.pieces.size} tokens: '${text.take(160)}'")
    }

    @Test
    fun cancelStopsGenerationAndKeepsWhatWasProduced() {
        loadOrFail()

        val c = Collector()
        var raw = ""
        // A large budget on purpose: if cancel() does nothing, the run keeps
        // going and the assertion on `generated` fails for the right reason
        // instead of the test quietly passing on a short answer.
        val worker = Thread {
            raw = LlamaBridge.generate("Once upon a time", 400, 0.8f, c)
        }
        worker.start()

        // Press Stop only once there is something to keep. This is the user's
        // sequence exactly: text appears, then Stop.
        assertTrue(
            "no token arrived, so there was nothing to cancel",
            c.awaitFirstPiece(600)
        )
        val tokensBeforeCancel = c.pieces.size
        Log.i(TAG, "pressing Stop after $tokensBeforeCancel tokens")
        LlamaBridge.cancel()
        worker.join(120_000)
        assertFalse("generate() never returned after cancel", worker.isAlive)

        val res = JSONObject(raw)
        assertTrue("generate failed: $raw", res.getBoolean("ok"))
        assertTrue("cancel was not honoured: $raw", res.getBoolean("cancelled"))
        assertTrue("onDone never fired after cancel", c.done.get())
        assertTrue(
            "cancel kept nothing, so the partial output was lost: $raw",
            c.pieces.isNotEmpty()
        )
        assertTrue(
            "generation ran past the cancel point (${res.getInt("generated")} of 400)",
            res.getInt("generated") < 400
        )
        Log.i(
            TAG,
            "cancelled after ${res.getInt("generated")} tokens " +
                "(budget 400), kept '${c.pieces.joinToString("").take(120)}'"
        )
    }

    @Test
    fun cancelBeforeAnyTokenStillTerminates() {
        loadOrFail()

        val c = Collector()
        var raw = ""
        // No piece has been produced yet when cancel lands: the flag has to be
        // noticed during PROMPT EVALUATION, not only while sampling. This is
        // the case a flag checked solely in the sampling loop misses.
        val thread = Thread {
            raw = LlamaBridge.generate(
                "A long prompt that must be tokenised and evaluated before a single " +
                    "sampled token exists, repeated so it is not trivially short. " +
                    "Repeated so it is not trivially short. Repeated so it is not " +
                    "trivially short. Repeated so it is not trivially short.",
                200, 0.8f, c
            )
        }
        thread.start()
        Thread.sleep(50)
        LlamaBridge.cancel()

        // Stop is cooperative: the flag is read between prefill batches, so the
        // worst case is one whole batch of prompt evaluation finishing first.
        // This guest is a TCG software-emulated x86_64 with no KVM, where a
        // ~40 token prefill batch takes minutes, not seconds, so the bound has
        // to be generous. The assertion is that the run ENDS, not that it ends
        // instantly: the point is that it ends at all and reports the stop,
        // rather than generating all 200 tokens as it would with no flag.
        thread.join(PREFILL_CANCEL_GRACE_MS)
        assertFalse(
            "generate() did not return ${PREFILL_CANCEL_GRACE_MS} ms after cancel",
            thread.isAlive
        )

        val res = JSONObject(raw)
        assertTrue("generate failed: $raw", res.getBoolean("ok"))
        assertTrue("cancel during prefill was not honoured: $raw", res.getBoolean("cancelled"))
        assertEquals(
            "a token escaped even though the stop was requested during prefill: $raw",
            0, res.getInt("generated")
        )
        assertEquals(
            "no token should have reached the callback: ${c.pieces}",
            0, c.pieces.size
        )
        Log.i(
            TAG,
            "early cancel honoured: prefill_batches=${res.getInt("prefill_batches")} " +
                "prefill_ms=${res.getLong("prefill_ms")} generated=${res.getInt("generated")}"
        )
    }

    @Test
    fun unloadIsSafeToRepeatAndAllowsReload() {
        loadOrFail()
        LlamaBridge.unload()
        loaded = false
        // Unloading twice must not crash: the Activity calls it from onDestroy,
        // which can run more than once across a configuration change.
        LlamaBridge.unload()

        val meta = JSONObject(LlamaBridge.load(modelPath, N_CTX))
        assertTrue("reload after unload failed: $meta", meta.getBoolean("ok"))
        loaded = true
        Log.i(TAG, "unload + reload OK")
    }

    private fun loadOrFail() {
        if (!loaded) {
            val meta = JSONObject(LlamaBridge.load(modelPath, N_CTX))
            assertTrue("load failed: $meta", meta.getBoolean("ok"))
            assertNotNull(meta.getString("architecture"))
            loaded = true
        }
    }
}
