#include <jni.h>
#include <android/log.h>
#include <stdio.h>
#include <time.h>
#include "hydra_model.h"
#include "hydra_batch.h"

#define TAG "HydraJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static JavaVM *g_vm = NULL;

/* Cooperative cancel.
 *
 * A JNI call cannot be interrupted from the UI thread, so Cancel cannot
 * abandon this loop from the outside. It sets this flag instead, and the
 * step wrapper checks it once per step: the run stops within one step and
 * returns {"cancelled":true}. This is the honest version - the button stops
 * the work, it does not merely disconnect the UI from the result.
 *
 * volatile, not _Atomic: the only requirement is that the write from the
 * UI thread becomes visible to the inference thread, and a per-step load of
 * a volatile object is exactly that. It is cleared at the start of every
 * run, so a stale cancel cannot kill the NEXT run. */
static volatile int g_cancel_requested = 0;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

/* Sets the cooperative cancel flag for the run in progress.
 *
 * Kotlin declares this as `external fun cancel()`; see tools/jni_signature_check.sh,
 * which derives the symbol from the Kotlin source and fails if the two drift apart. */
JNIEXPORT void JNICALL
Java_dev_hydrastone_HydraBridge_cancel(JNIEnv *env, jobject self)
{
    (void)env;
    (void)self;
    g_cancel_requested = 1;
    LOGI("cancel requested");
}

/* Step wrapper for hydra_run_steps(): one engine step, or a refusal when
 * Cancel was pressed. The generated token is fed back in as the next
 * input, which is the same chaining the unbatched loop did. */
typedef struct {
    HydraEngine *engine;
    uint16_t tok;
} StepCtx;

static int jni_step(void *user, uint16_t *token_out)
{
    StepCtx *ctx = (StepCtx *)user;
    if (g_cancel_requested) return -1;
    if (hydra_engine_step(ctx->engine, ctx->tok, token_out) != 0) return -1;
    ctx->tok = *token_out;
    return 0;
}

/* Same step, but immune to the cancel flag: the benchmark must measure a
 * full run or report a failure, never a user-interrupted one. */
static int bench_step_no_cancel(void *user, uint16_t *token_out)
{
    StepCtx *ctx = (StepCtx *)user;
    if (hydra_engine_step(ctx->engine, ctx->tok, token_out) != 0) return -1;
    ctx->tok = *token_out;
    return 0;
}

/* Einen Block erzeugter Tokens an Kotlin uebergeben:
 *   callback.onTokens(int[] tokens, boolean done)
 * Gibt 0 zurueck, -1 wenn der Kotlin-Callback geworfen hat (dann muss der
 * Stream abbrechen - eine pending Exception macht jeden weiteren JNI-Call
 * undefiniert).
 *
 * Wann ein Block den Weg nach Kotlin geht, entscheidet hydra_run_steps()
 * in hydra_batch.h - dieselbe Schleife, die tests/test_jni_batch.c auf dem
 * Host laufen laesst. */
static int emit_tokens(JNIEnv *env, jobject callback, const uint16_t *tokens,
                       int n, jboolean done)
{
    if (!env || !callback) return 0;
    jclass cls = (*env)->GetObjectClass(env, callback);
    if (!cls) return -1;
    jmethodID mid = (*env)->GetMethodID(env, cls, "onTokens", "([IZ)V");
    if (!mid) {
        LOGE("onTokens(int[], boolean) not found on the callback");
        (*env)->DeleteLocalRef(env, cls);
        return -1;
    }

    /* jintArray, nicht jshortArray: Kotlin Int ist immer 32 Bit, und ein
     * jshort[] wuerde stillschweigend die Haelfte der Token-Werte
     * zerstoeren (ein Token kann 0..1023 sein, aber der Puffer ist
     * uint16_t - jeder Wert >= 32768 wuerde negativ). */
    jintArray arr = (*env)->NewIntArray(env, n);
    if (!arr) {
        LOGE("NewIntArray failed for %d tokens", n);
        (*env)->DeleteLocalRef(env, cls);
        return -1;
    }
    /* Ein lokales jint-Array als Puffer: GetIntArrayRegion kopiert direkt
     * hinein, kein zweiter malloc. */
    {
        static jint staging[HYDRA_JNI_BATCH];
        int m = n < HYDRA_JNI_BATCH ? n : HYDRA_JNI_BATCH;
        int i;
        for (i = 0; i < m; ++i) staging[i] = (jint)tokens[i];
        (*env)->SetIntArrayRegion(env, arr, 0, m, staging);
    }
    (*env)->CallVoidMethod(env, callback, mid, arr, done);
    (*env)->DeleteLocalRef(env, arr);

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        LOGE("onTokens callback threw - aborting token stream");
        (*env)->DeleteLocalRef(env, cls);
        return -1;
    }
    (*env)->DeleteLocalRef(env, cls);
    return 0;
}

/* Context for the emit side of hydra_run_steps(). */
typedef struct {
    JNIEnv *env;
    jobject callback;
} EmitCtx;

static int jni_emit(void *user, const uint16_t *tokens, int n, int done)
{
    EmitCtx *ctx = (EmitCtx *)user;
    return emit_tokens(ctx->env, ctx->callback, tokens, n, done ? JNI_TRUE : JNI_FALSE);
}

/*
 * Runs inference against a model file on local storage.
 *
 * @param modelPath  absolute path to a .hydra file (copied from assets or
 *                   imported via SAF)
 * @param prompt     int[] of token ids fed through the engine BEFORE the
 *                   generation (may be null or empty). The last entry is
 *                   the seed for the first generated token, so a prompt
 *                   really is context and not just a start value.
 * @param steps      number of generated inference steps (1..256). A value
 *                   below 1 is REJECTED, not clamped: clamping made a
 *                   zero-step run report success with no tokens at all.
 * @param callback   instance of dev.hydrastone.HydraBridge.Callback
 * @return JSON-ish summary string: {"ok":true,"steps":N,"elapsed_us":X,...}
 *         or {"ok":false,"error":"..."}
 *
 * The state vector returned in the summary is NOT included: this path uses
 * hydra_engine_step(), which updates the whole vector. If it is ever
 * switched to hydra_engine_step_fast() (token-only), state[1..dim-1] stays
 * stale by design and no UI may render a full state strip from it - see the
 * note in include/hydra_model.h.
 */
JNIEXPORT jstring JNICALL
Java_dev_hydrastone_HydraBridge_runInference(
        JNIEnv *env, jclass clazz,
        jstring modelPath, jintArray prompt, jint steps, jobject callback)
{
    (void)clazz;

    /* Refuse before anything else: no file is mapped, no engine is stepped,
     * no block is emitted. Checked here and again inside hydra_run_steps(),
     * which is the version the host test exercises. */
    if (steps < 1) {
        LOGE("runInference rejected: steps=%d (must be >= 1)", (int)steps);
        return (*env)->NewStringUTF(env,
            "{\"ok\":false,\"error\":\"steps must be >= 1\"}");
    }
    if (steps > 256) steps = 256;

    /* A cancel pressed between two runs must not kill this one. */
    g_cancel_requested = 0;

    const char *path = (*env)->GetStringUTFChars(env, modelPath, NULL);
    if (!path) {
        return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"path null\"}");
    }

    /* Prompt-Token vor dem Laden kopieren: nach hydra_engine_load() wird
     * das jintArray-Handle weiter unten womoeglich als Lokal-Referenz
     * wegoptimiert. Ausserdem wird jeder Token gegen 0..65535 geprueft -
     * ein negatives oder zu grosses Token waere ein still falscher Seed. */
    uint16_t prompt_buf[256];
    size_t prompt_n = 0;
    if (prompt != NULL) {
        jsize plen = (*env)->GetArrayLength(env, prompt);
        if (plen > (jsize)(sizeof(prompt_buf) / sizeof(prompt_buf[0]))) {
            LOGE("prompt too long: %d tokens (max %zu)", (int)plen,
                 sizeof(prompt_buf) / sizeof(prompt_buf[0]));
            (*env)->ReleaseStringUTFChars(env, modelPath, path);
            return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"prompt too long\"}");
        }
        for (jsize i = 0; i < plen; ++i) {
            jint v = 0;
            (*env)->GetIntArrayRegion(env, prompt, i, 1, &v);
            if (v < 0 || v > 65535) {
                LOGE("prompt token %d out of range: %d", (int)i, (int)v);
                (*env)->ReleaseStringUTFChars(env, modelPath, path);
                return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"prompt token out of range\"}");
            }
            prompt_buf[prompt_n++] = (uint16_t)v;
        }
    }

    HydraEngine engine;
    int rc = hydra_engine_load(&engine, path);
    (*env)->ReleaseStringUTFChars(env, modelPath, path);
    if (rc != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"load rc=%d\"}", rc);
        LOGE("hydra_engine_load failed rc=%d", rc);
        return (*env)->NewStringUTF(env, buf);
    }

    long t0 = clock();
    /* Header-Werte VOR unload() lesen (unload memset't die Struktur) */
    const unsigned dim = engine.header.dim;
    const unsigned vocab = engine.header.vocab_size;
    const unsigned layers = engine.header.layers;
    int blocks_count = 0;
    int tokens_total = 0;

    /* Prompt (alle Token ausser dem letzten) durch die Engine schicken. */
    if (prompt_n > 0) {
        if (hydra_engine_prefill(&engine, prompt_buf, prompt_n) != 0) {
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"prefill failed\"}");
        }
    }

    /* Seed: letzter Prompt-Token, sonst Token 0. Ein Token ausserhalb des
     * Vokabulars wird wie in der Engine modularisiert, nicht abgelehnt -
     * das ist die dokumentierte Semantik von hydra_engine_step(). */
    uint16_t tok = prompt_n > 0 ? prompt_buf[prompt_n - 1] : 0u;
    if (tok >= vocab) tok = (uint16_t)(tok % vocab);

    /* Der Blockversand liegt in hydra_batch.h, damit die Regel
     * "auch ein unvollstaendiger letzter Block wird mit done=true gesendet"
     * auf dem Host getestet werden kann (tests/test_jni_batch.c). */
    {
        HydraBatch batch;
        StepCtx step_ctx;
        EmitCtx emit_ctx;
        int blocks = 0, total = 0;
        int rc;

        step_ctx.engine = &engine;
        step_ctx.tok = tok;
        emit_ctx.env = env;
        emit_ctx.callback = callback;

        rc = hydra_run_steps(&batch, (int)steps, jni_step, &step_ctx,
                             jni_emit, &emit_ctx, &blocks, &total);
        if (rc == HYDRA_RUN_STEP_ERR && g_cancel_requested) {
            hydra_engine_unload(&engine);
            LOGI("inference cancelled after %d step(s), %d block(s)", total, blocks);
            return (*env)->NewStringUTF(env,
                "{\"ok\":false,\"cancelled\":true,\"error\":\"cancelled by the user\"}");
        }
        if (rc == HYDRA_RUN_BAD_STEPS) {
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env,
                "{\"ok\":false,\"error\":\"steps must be >= 1\"}");
        }
        if (rc == HYDRA_RUN_STEP_ERR) {
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"step failed\"}");
        }
        if (rc == HYDRA_RUN_EMIT_ERR) {
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"callback failed\"}");
        }
        blocks_count = blocks;
        tokens_total = total;
    }
    long elapsed_us = (clock() - t0) * 1000000L / CLOCKS_PER_SEC;

    /* Axiom gate starter (mirrors the CLI smoke test) */
    float safe = 0.0f;
    int allowed = hydra_verify_axiom(1.0f, 99.5f, &safe);

    hydra_engine_unload(&engine);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"steps\":%d,\"emitted\":%d,\"elapsed_us\":%ld,"
             "\"dim\":%u,\"vocab\":%u,\"layers\":%u,\"prompt\":%zu,\"batches\":%d,"
             "\"axiom_allowed\":%s}",
             (int)steps, tokens_total, elapsed_us, dim, vocab, layers, prompt_n,
             blocks_count,
             allowed ? "true" : "false");
    LOGI("inference done: %s", buf);
    return (*env)->NewStringUTF(env, buf);
}

/*
 * Phase 0 device benchmark: three costs on the SAME model and the SAME
 * number of steps, so the numbers are comparable.
 *
 *   native_only    : the engine loop in this process, no JNI at all.
 *   jni_per_token  : one GetMethodID + CallVoidMethod per token (the old
 *                    callback shape). Kept deliberately, because the point
 *                    of the measurement is the delta against batching.
 *   jni_batched    : the shipping path — one callback per 16 tokens plus
 *                    one final done=true.
 *
 * The callbacks here only count; the real UI callback does more work, so
 * these numbers are a lower bound for the app, which is stated in the
 * returned JSON rather than glossed over.
 *
 * Timing uses clock_gettime(CLOCK_MONOTONIC), not clock(): clock() measures
 * CPU time and would hide any driver wait time — exactly the time a GPU
 * comparison needs to see.
 */
#define HYDRA_BENCH_STEPS_DEFAULT 2000

static volatile int g_bench_sink = 0;

static int bench_native_only(HydraEngine *e, uint16_t tok, int steps)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < steps; ++i) {
        uint16_t next = 0;
        if (hydra_engine_step(e, tok, &next) != 0) return -1;
        tok = next;
        g_bench_sink += next;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (int)((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec));
}

static int bench_jni_per_token(JNIEnv *env, jobject callback, HydraEngine *e,
                               uint16_t tok, int steps)
{
    jclass cls = (*env)->GetObjectClass(env, callback);
    if (!cls) return -1;
    /* (II)V is the OLD per-token signature on purpose. */
    jmethodID mid = (*env)->GetMethodID(env, cls, "onToken", "(II)V");
    if (!mid) { (*env)->DeleteLocalRef(env, cls); return -2; }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < steps; ++i) {
        uint16_t next = 0;
        if (hydra_engine_step(e, tok, &next) != 0) { (*env)->DeleteLocalRef(env, cls); return -3; }
        (*env)->CallVoidMethod(env, callback, mid, i, next);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); (*env)->DeleteLocalRef(env, cls); return -4; }
        tok = next;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (*env)->DeleteLocalRef(env, cls);
    return (int)((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec));
}

/* The batched benchmark runs the SHIPPED loop (hydra_run_steps), not a copy
 * of it: a benchmark that measures a second implementation measures the
 * wrong thing the moment the two drift apart. The per-token variant below
 * deliberately keeps its own loop, because that is what it is measuring. */
static int bench_jni_batched(JNIEnv *env, jobject callback, HydraEngine *e,
                             uint16_t tok, int steps)
{
    struct timespec t0, t1;
    HydraBatch batch;
    StepCtx step_ctx;
    EmitCtx emit_ctx;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    step_ctx.engine = e;
    step_ctx.tok = tok;
    emit_ctx.env = env;
    emit_ctx.callback = callback;

    /* The cancel flag must not be able to end a benchmark half-way: this
     * is a measurement, not a user request. */
    if (hydra_run_steps(&batch, steps, bench_step_no_cancel, &step_ctx,
                        jni_emit, &emit_ctx, NULL, NULL) != HYDRA_RUN_OK) {
        return -4;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (int)((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec));
}

JNIEXPORT jstring JNICALL
Java_dev_hydrastone_HydraBridge_benchmark(
        JNIEnv *env, jclass clazz,
        jstring modelPath, jint steps, jobject callback)
{
    (void)clazz;
    if (steps < 1) steps = HYDRA_BENCH_STEPS_DEFAULT;
    if (steps > 200000) steps = 200000;

    const char *path = (*env)->GetStringUTFChars(env, modelPath, NULL);
    if (!path) return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"path null\"}");

    double ns_native = -1, ns_per_token = -1, ns_batched = -1;
    unsigned dim = 0, layers = 0, vocab = 0;
    const uint16_t seed = 42;
    char err[128];

    for (int pass = 0; pass < 3; ++pass) {
        HydraEngine e;
        int rc = hydra_engine_load(&e, path);
        if (rc != 0) {
            snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"load rc=%d\"}", rc);
            (*env)->ReleaseStringUTFChars(env, modelPath, path);
            return (*env)->NewStringUTF(env, err);
        }
        dim = e.header.dim; layers = e.header.layers; vocab = e.header.vocab_size;

        /* Pass 0 warms the mmap pages for every path that follows - the
         * same warmup discipline as the CLI benchmark. */
        if (pass == 0) {
            uint16_t t = seed;
            for (int i = 0; i < (int)e.header.layers; ++i) {
                uint16_t n2 = 0;
                if (hydra_engine_step(&e, t, &n2) != 0) break;
                t = n2;
            }
        } else if (pass == 1) {
            int ns = bench_native_only(&e, seed, steps);
            ns_native = ns > 0 ? (double)ns / steps : -1;
        } else if (pass == 2 && callback) {
            int ns = bench_jni_per_token(env, callback, &e, seed, steps);
            ns_per_token = ns > 0 ? (double)ns / steps : -1;
            hydra_engine_unload(&e);
            rc = hydra_engine_load(&e, path);
            if (rc != 0) {
                snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"reload failed\"}");
                (*env)->ReleaseStringUTFChars(env, modelPath, path);
                return (*env)->NewStringUTF(env, err);
            }
            int ns2 = bench_jni_batched(env, callback, &e, seed, steps);
            ns_batched = ns2 > 0 ? (double)ns2 / steps : -1;
        }
        hydra_engine_unload(&e);
    }
    (*env)->ReleaseStringUTFChars(env, modelPath, path);

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"steps\":%d,\"dim\":%u,\"layers\":%u,\"vocab\":%u,"
             "\"ns_per_token_native_only\":%.1f,"
             "\"ns_per_token_jni_per_token\":%.1f,"
             "\"ns_per_token_jni_batched\":%.1f,"
             "\"jni_overhead_per_token\":%.1f,"
             "\"batching_saves_per_token\":%.1f,"
             "\"callback_does\":\"count only, so the UI cost is additional\"}",
             (int)steps, dim, layers, vocab,
             ns_native, ns_per_token, ns_batched,
             ns_per_token - ns_native, ns_per_token - ns_batched);
    return (*env)->NewStringUTF(env, buf);
}
