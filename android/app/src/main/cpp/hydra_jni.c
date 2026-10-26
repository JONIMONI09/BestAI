#include <jni.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include "hydra_model.h"
#include "hydra_batch.h"
#include "hydra_sample_cfg.h"

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
 * Runs inference with the C SAMPLER (hydra_sample_token) exposed to the
 * app. Deliberate SIBLING of runInference(), not a shared refactor of it:
 * the default (greedy) path of runInference() must stay bit-identical to
 * the pre-sampler baseline that tools/jni_signature_check.sh and the
 * deterministic engine tests pin, so that function is untouched.
 *
 * What is new here (and only here):
 *   - the step callback is hydra_sampled_step() (src/hydra_engine.c),
 *     the ONE dispatch the CLI and the host tests use:
 *       sampling on  -> hydra_sample_token() + recent-token window,
 *       greedy, v3   -> hydra_engine_step_tf() + caller-owned KV cache,
 *       greedy, v1/v2-> hydra_engine_step() (the pre-sampler baseline);
 *   - version-3 models run through this method at all: runInference()
 *     still uses hydra_engine_step(), which refuses v3 (rc -14). The KV
 *     ring (cap = steps + 4, one sink) is allocated like the CLI does,
 *     advanced by every step, and freed before the answer is built;
 *   - the sampler parameters arrive as primitive JNI args (no new
 *     callback/interface shape) and are validated here, mirroring the
 *     engine's own ranges: temperature 0..20 (0 = greedy guard),
 *     min_p 0..1 (1 = greedy guard), repetition_penalty >= 1.0,
 *     NaN/inf rejected up front (a NaN parameter must produce an error
 *     string, not a silent greedy run);
 *   - the penalty window is fixed at 64, the CLI default, and the RNG
 *     seed is passed through (0 maps to the engine's non-zero
 *     fallback constant, so it is reproducible, never degenerate).
 *
 * Greedy compatibility: with temperature <= 0 (or min_p >= 1) the
 * engine's greedy guard delegates to the native step, so the token
 * stream of this method with greedy settings is bit-identical to
 * runInference() on v1/v2 and to the CLI's v3 run. tests/test_engine.c
 * pins that property on the host (test_jni_sampled_*).
 *
 * Kotlin declares this as `external fun runInferenceSampled(...)`; see
 * tools/jni_signature_check.sh, which derives the symbol from the
 * Kotlin source and fails if the two drift apart.
 */
JNIEXPORT jstring JNICALL
Java_dev_hydrastone_HydraBridge_runInferenceSampled(
        JNIEnv *env, jclass clazz,
        jstring modelPath, jintArray prompt, jint steps, jobject callback,
        jfloat temperature, jfloat minP, jfloat repPenalty, jint seed)
{
    (void)clazz;

    if (steps < 1) {
        LOGE("runInferenceSampled rejected: steps=%d (must be >= 1)",
             (int)steps);
        return (*env)->NewStringUTF(env,
            "{\"ok\":false,\"error\":\"steps must be >= 1\"}");
    }
    if (steps > 256) steps = 256;

    const float temp = (float)temperature;
    const float mp = (float)minP;
    const float rp = (float)repPenalty;
    /* The range rules live in hydra_sample_cfg.h (the same header the
     * host test compiles); this file only maps the result to the JSON
     * error the user sees. Check order is the header's contract order. */
    const int scfg = hydra_sample_cfg_validate(temp, mp, rp);
    if (scfg != HYDRA_SAMPLE_OK) {
        const char *err =
            (scfg == HYDRA_SAMPLE_BAD_TEMP)
                ? "temperature out of range (0..20, 0 = greedy)"
                : (scfg == HYDRA_SAMPLE_BAD_MINP)
                      ? "min-p out of range (0..1)"
                      : "repetition-penalty must be >= 1.0";
        LOGE("runInferenceSampled rejected: %s (temp=%f minP=%f rp=%f)",
             err, temp, mp, rp);
        char out[160];
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", err);
        return (*env)->NewStringUTF(env, out);
    }

    /* A cancel pressed between two runs must not kill this one. */
    g_cancel_requested = 0;

    const char *path = (*env)->GetStringUTFChars(env, modelPath, NULL);
    if (!path) {
        return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"path null\"}");
    }

    /* Prompt copy + range check: same rules as runInference() - a
     * negative or > 65535 token would be a silently wrong seed. */
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
    const unsigned dim = engine.header.dim;
    const unsigned vocab = engine.header.vocab_size;
    const unsigned layers = engine.header.layers;

    /* Version 3/4: the caller owns the KV storage (the engine stays
     * zero-heap). v3 uses one shared ring; v4 one cache PER LAYER
     * (every layer has its own Wq/Wk/Wv and attends to its own
     * memory). cap = steps + 4 + one sink covers the whole run,
     * exactly like the CLI does with 512 for its 256-step max. */
    const uint32_t n_kv =
        (uint32_t)((engine.header.version == 4) ? layers
                        : (engine.header.version == 3) ? 1u : 0u);
    float *kv_k = NULL, *kv_v = NULL;
    hydra_kv_cache_t *kv_arr = NULL;
    hydra_kv_cache_t *kv = NULL;
    hydra_kv_cache_t *kv_layers = NULL;
    if (n_kv > 0) {
        const uint32_t cap = (uint32_t)steps + 4, nsink = 1;
        const size_t per = ((size_t)cap + nsink) * (size_t)dim * sizeof(float);
        kv_k = (float *)malloc(per * (size_t)n_kv);
        kv_v = (float *)malloc(per * (size_t)n_kv);
        kv_arr = (hydra_kv_cache_t *)calloc(n_kv, sizeof(hydra_kv_cache_t));
        if (!kv_k || !kv_v || !kv_arr) {
            free(kv_k); free(kv_v); free(kv_arr);
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env,
                "{\"ok\":false,\"error\":\"kv cache init failed\"}");
        }
        for (uint32_t l = 0; l < n_kv; ++l) {
            if (hydra_kv_cache_init(&kv_arr[l],
                    kv_k + (size_t)l * ((size_t)cap + nsink) * (size_t)dim,
                    kv_v + (size_t)l * ((size_t)cap + nsink) * (size_t)dim,
                    dim, cap, nsink) != 0) {
                free(kv_k); free(kv_v); free(kv_arr);
                hydra_engine_unload(&engine);
                return (*env)->NewStringUTF(env,
                    "{\"ok\":false,\"error\":\"kv cache init failed\"}");
            }
        }
        kv = (engine.header.version == 3) ? &kv_arr[0] : NULL;
        kv_layers = (engine.header.version == 4) ? kv_arr : NULL;
    }

    /* The generated-token log hydra_sampled_step() reads back for the
     * repetition penalty: capacity = steps, so gen[s] always fits. */
    uint16_t *gen = (uint16_t *)malloc((size_t)steps * sizeof(uint16_t));
    if (!gen) {
        free(kv_k); free(kv_v); free(kv_arr);
        hydra_engine_unload(&engine);
        return (*env)->NewStringUTF(env,
            "{\"ok\":false,\"error\":\"alloc failed\"}");
    }

    /* Prefill + seed: same semantics as runInference() - the last
     * prompt token is the seed, out-of-vocab values are modularised
     * (documented engine semantics), not rejected. */
    if (prompt_n > 0) {
        if (hydra_engine_prefill(&engine, prompt_buf, prompt_n) != 0) {
            free(gen); free(kv_k); free(kv_v); free(kv_arr);
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"prefill failed\"}");
        }
    }
    uint16_t tok = prompt_n > 0 ? prompt_buf[prompt_n - 1] : 0u;
    if (tok >= vocab) tok = (uint16_t)(tok % vocab);

    hydra_sampled_step_ctx_t step_ctx;
    step_ctx.engine = &engine;
    step_ctx.tok = tok;
    step_ctx.cfg.temperature = temp;
    step_ctx.cfg.min_p = mp;
    step_ctx.cfg.repetition_penalty = rp;
    step_ctx.cfg.penalty_window = 64; /* CLI default; 0 would disable it */
    step_ctx.cfg.rng_seed = (uint32_t)seed;
    step_ctx.gen = gen;
    step_ctx.s = 0;
    step_ctx.kv = kv;
    step_ctx.kv_layers = kv_layers;

    EmitCtx emit_ctx;
    emit_ctx.env = env;
    emit_ctx.callback = callback;

    int blocks_count = 0;
    int tokens_total = 0;
    {
        HydraBatch batch;
        int blocks = 0, total = 0;

        rc = hydra_run_steps(&batch, (int)steps, hydra_sampled_step,
                             &step_ctx, jni_emit, &emit_ctx, &blocks, &total);
        if (rc == HYDRA_RUN_STEP_ERR && g_cancel_requested) {
            free(gen); free(kv_k); free(kv_v); free(kv_arr);
            hydra_engine_unload(&engine);
            LOGI("sampled inference cancelled after %d step(s), %d block(s)",
                 total, blocks);
            return (*env)->NewStringUTF(env,
                "{\"ok\":false,\"cancelled\":true,\"error\":\"cancelled by the user\"}");
        }
        if (rc == HYDRA_RUN_BAD_STEPS) {
            free(gen); free(kv_k); free(kv_v); free(kv_arr);
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env,
                "{\"ok\":false,\"error\":\"steps must be >= 1\"}");
        }
        if (rc == HYDRA_RUN_STEP_ERR) {
            free(gen); free(kv_k); free(kv_v); free(kv_arr);
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"step failed\"}");
        }
        if (rc == HYDRA_RUN_EMIT_ERR) {
            free(gen); free(kv_k); free(kv_v); free(kv_arr);
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"callback failed\"}");
        }
        blocks_count = blocks;
        tokens_total = total;
    }
    long elapsed_us = (clock() - t0) * 1000000L / CLOCKS_PER_SEC;

    uint32_t kv_count = 0;
    for (uint32_t l = 0; l < n_kv; ++l)
        kv_count += hydra_kv_cache_size(&kv_arr[l]);
    const int sampling_on = (temp > 0.0f && mp < 1.0f);
    const int version = engine.header.version; /* read BEFORE unload: it memset's */

    /* Axiom gate starter (mirrors the CLI smoke test) */
    float safe = 0.0f;
    int allowed = hydra_verify_axiom(1.0f, 99.5f, &safe);

    free(gen);
    free(kv_k);
    free(kv_v);
    free(kv_arr);
    hydra_engine_unload(&engine);

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"steps\":%d,\"emitted\":%d,\"elapsed_us\":%ld,"
             "\"dim\":%u,\"vocab\":%u,\"layers\":%u,\"prompt\":%zu,\"batches\":%d,"
             "\"version\":%d,\"kv_count\":%u,"
             "\"sampling\":{\"mode\":\"%s\",\"temperature\":%.6g,\"min_p\":%.6g,"
             "\"repetition_penalty\":%.6g,\"penalty_window\":64,\"seed\":%u},"
             "\"axiom_allowed\":%s}",
             (int)steps, tokens_total, elapsed_us, dim, vocab, layers, prompt_n,
             blocks_count, version, kv_count,
             sampling_on ? "sampled" : "greedy",
             (double)temp, (double)mp, (double)rp, (unsigned)step_ctx.cfg.rng_seed,
             allowed ? "true" : "false");
    LOGI("sampled inference done: %s", buf);
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
