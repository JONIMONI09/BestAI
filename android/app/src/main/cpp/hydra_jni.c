#include <jni.h>
#include <android/log.h>
#include <stdio.h>
#include <time.h>
#include "hydra_model.h"

#define TAG "HydraJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static JavaVM *g_vm = NULL;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

/* Stream one generated token to Kotlin via a Java callback:
 * callback.onToken(int step, int token) */
static void emit_token(JNIEnv *env, jobject callback, int step, int token)
{
    if (!env || !callback) return;
    jclass cls = (*env)->GetObjectClass(env, callback);
    if (!cls) return;
    jmethodID mid = (*env)->GetMethodID(env, cls, "onToken", "(II)V");
    if (mid) {
        (*env)->CallVoidMethod(env, callback, mid, step, token);
    }
    (*env)->DeleteLocalRef(env, cls);
}

/*
 * Runs inference against a model file on local storage.
 *
 * @param modelPath  absolute path to a .hydra file (copied from assets)
 * @param startToken seed token
 * @param steps      number of inference steps (1..256, clamped)
 * @param callback   instance of dev.hydrastone.HydraBridge.Callback
 * @return JSON-ish summary string: {"ok":true,"steps":N,"ms":X,"model":...}
 *         or {"ok":false,"error":"..."}
 */
JNIEXPORT jstring JNICALL
Java_dev_hydrastone_HydraBridge_runInference(
        JNIEnv *env, jclass clazz,
        jstring modelPath, jint startToken, jint steps, jobject callback)
{
    (void)clazz;
    const char *path = (*env)->GetStringUTFChars(env, modelPath, NULL);
    if (!path) {
        return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"path null\"}");
    }

    if (steps < 1) steps = 1;
    if (steps > 256) steps = 256;

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

    uint16_t tok = (uint16_t)(startToken % vocab);
    for (int s = 0; s < steps; ++s) {
        uint16_t next = 0;
        if (hydra_engine_step(&engine, tok, &next) != 0) {
            hydra_engine_unload(&engine);
            return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"step failed\"}");
        }
        emit_token(env, callback, s, next);
        tok = next;
    }
    long elapsed_us = (clock() - t0) * 1000000L / CLOCKS_PER_SEC;

    /* Axiom gate demo (mirrors the CLI smoke test) */
    float safe = 0.0f;
    int allowed = hydra_verify_axiom(1.0f, 99.5f, &safe);

    hydra_engine_unload(&engine);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"steps\":%d,\"elapsed_us\":%ld,\"dim\":%u,"
             "\"vocab\":%u,\"layers\":%u,\"axiom_allowed\":%s}",
             (int)steps, elapsed_us, dim, vocab, layers,
             allowed ? "true" : "false");
    LOGI("inference done: %s", buf);
    return (*env)->NewStringUTF(env, buf);
}
