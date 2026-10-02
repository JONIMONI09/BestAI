#include <jni.h>
#include <android/log.h>
#include <stdio.h>
#include <time.h>
#include "hydra_model.h"

#define TAG "HydraJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static JavaVM *g_vm = NULL;

/* Wie viele Tokens pro JNI-Aufruf gebuendelt werden. Der pro-Token-Ubergang
 * (JNI-Aufruf + Kotlin-Objektmethoden-Dispatch) kostet bei winziger Engine
 * mehr Zeit als die Engine selbst; 16 ist ein Kompromiss aus sichtbarer
 * Fortschrittsanzeige und geringer Aufrufzahl. */
#define HYDRA_JNI_BATCH 16

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

/* Einen Block erzeugter Tokens an Kotlin uebergeben:
 *   callback.onTokens(int[] tokens, boolean done)
 * Ein Aufruf pro HYDRA_JNI_BATCH Tokens und ein letzter mit done=true.
 * Gibt 0 zurueck, -1 wenn der Kotlin-Callback geworfen hat (dann muss der
 * Stream abbrechen - eine pending Exception macht jeden weiteren JNI-Call
 * undefiniert). */
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
        for (int i = 0; i < m; ++i) staging[i] = (jint)tokens[i];
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

/*
 * Runs inference against a model file on local storage.
 *
 * @param modelPath  absolute path to a .hydra file (copied from assets or
 *                   imported via SAF)
 * @param prompt     int[] of token ids fed through the engine BEFORE the
 *                   generation (may be null or empty). The last entry is
 *                   the seed for the first generated token, so a prompt
 *                   really is context and not just a start value.
 * @param steps      number of generated inference steps (1..256, clamped)
 * @param callback   instance of dev.hydrastone.HydraBridge.Callback
 * @return JSON-ish summary string: {"ok":true,"steps":N,"elapsed_us":X,...}
 *         or {"ok":false,"error":"..."}
 */
JNIEXPORT jstring JNICALL
Java_dev_hydrastone_HydraBridge_runInference(
        JNIEnv *env, jclass clazz,
        jstring modelPath, jintArray prompt, jint steps, jobject callback)
{
    (void)clazz;
    const char *path = (*env)->GetStringUTFChars(env, modelPath, NULL);
    if (!path) {
        return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"path null\"}");
    }

    if (steps < 1) steps = 1;
    if (steps > 256) steps = 256;

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

    uint16_t batch[HYDRA_JNI_BATCH];
    int batch_n = 0;
    for (int s = 0; s < steps; ++s) {
        uint16_t next = 0;
        if (hydra_engine_step(&engine, tok, &next) != 0) {
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"step failed\"}");
        }
        batch[batch_n++] = next;
        tok = next;
        if (batch_n == HYDRA_JNI_BATCH) {
            if (emit_tokens(env, callback, batch, batch_n, JNI_FALSE) != 0) {
                LOGE("inference aborted at step %d after callback failure", s);
                hydra_engine_unload(&engine);
                return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"callback failed\"}");
            }
            batch_n = 0;
        }
    }
    /* Restlicher Block mit done=true - genau ein Abschluss je Lauf. */
    if (emit_tokens(env, callback, batch, batch_n, JNI_TRUE) != 0) {
        hydra_engine_unload(&engine);
        return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"callback failed\"}");
    }
    long elapsed_us = (clock() - t0) * 1000000L / CLOCKS_PER_SEC;

    /* Axiom gate demo (mirrors the CLI smoke test) */
    float safe = 0.0f;
    int allowed = hydra_verify_axiom(1.0f, 99.5f, &safe);

    hydra_engine_unload(&engine);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"steps\":%d,\"elapsed_us\":%ld,\"dim\":%u,"
             "\"vocab\":%u,\"layers\":%u,\"prompt\":%zu,\"batches\":%d,"
             "\"axiom_allowed\":%s}",
             (int)steps, elapsed_us, dim, vocab, layers, prompt_n,
             (int)((steps + HYDRA_JNI_BATCH - 1) / HYDRA_JNI_BATCH) + 1,
             allowed ? "true" : "false");
    LOGI("inference done: %s", buf);
    return (*env)->NewStringUTF(env, buf);
}
