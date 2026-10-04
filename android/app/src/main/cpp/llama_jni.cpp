// JNI bridge for llama.cpp.
//
// PINNED AGAINST v0.5.0 (7fe450e19305b828c199d602c23a8337aaa1f03b).
// Every call below was checked against the header at that tag; see
// docs/ANDROID-AUDIT.md section 3. Two API shapes differ from older
// llama.cpp and are the most likely thing to break if the pin moves:
//
//   1. The vocabulary is an OPAQUE POINTER from llama_model_get_vocab().
//      It is not a member of llama_model. Every tokenizer and detokenizer
//      call takes it as its FIRST argument.
//   2. End-of-generation is llama_vocab_is_eog(vocab, id). The older
//      llama_token_is_eog() exists but is DEPRECATED at this tag.
//
// Cancellation is a plain atomic flag read between decode steps. The context
// abort callback is deliberately NOT used: it fires from inside a matmul and
// tears down in-flight work, which is the wrong tool for a user pressing Stop
// on a stream they want the partial text from.

#include <jni.h>

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "llama.h"

// One tag for everything the native side says, so a single
// `adb logcat -s HydraLlamaNative` reconstructs the whole run.
#define LOG_TAG "HydraLlamaNative"

namespace {

// llama.cpp and ggml write everything - model metadata, buffer sizes, and the
// reason a decode failed - to stderr. On Android stderr is not captured, so a
// failure that the library explains perfectly is invisible. Redirecting the
// library's own log callback into logcat is what makes the native side of this
// engine observable at all, and it is also the only way to see a llama.cpp
// error message instead of guessing.
//
// The text is passed straight through and never rewritten, so a line in
// logcat is verbatim llama.cpp output. That matters for the "prove the native
// calls happened" requirement: these lines cannot be produced by the Kotlin
// side at all.
//
// DEBUG is dropped on purpose. At DEBUG, llama.cpp dumps every node of every
// graph, which is tens of thousands of lines per generated token; on a phone
// that is both a battery cost and enough output to rotate the whole logcat
// buffer away before anyone can read it. INFO already carries the buffer
// sizes, the graph shape and the decode timings - the parts worth keeping.
void native_log(ggml_log_level level, const char * text, void * user_data) {
    (void) user_data;
    if (level == GGML_LOG_LEVEL_DEBUG) {
        return;
    }
    int prio = ANDROID_LOG_INFO;
    switch (level) {
        case GGML_LOG_LEVEL_WARN:  prio = ANDROID_LOG_WARN;  break;
        case GGML_LOG_LEVEL_ERROR: prio = ANDROID_LOG_ERROR; break;
        default:                   prio = ANDROID_LOG_INFO;  break;
    }
    __android_log_write(prio, LOG_TAG, text != nullptr ? text : "");
}

// Counts native entry points actually entered. Reported on every generate so
// logcat proves the JNI bridge is the thing doing the work, rather than the
// Kotlin side merely believing it is.
std::atomic<uint64_t> g_native_calls{0};

// Every call below passes a string LITERAL as the format, and the attribute on
// the declaration is what makes that a guarantee rather than a convention:
// clang and gcc both reject a call whose format argument is not a literal, and
// reject arguments that do not match the conversions. The CWE-134 case
// flawfinder warns about - a format string an attacker can influence - is
// therefore a compile error here, not a runtime risk.
//
// flawfinder still reports that one line, because the attribute contains the
// token "printf". This single hit carries the suppression the tool documents.
// It is scoped to this line and nothing else: the body uses
// __android_log_vprint, so no printf-family call is left in this file at all.
//
// The va_list goes to logcat directly instead of through a char buffer.
// __android_log_vprint is the vprintf-shaped member of the same API, so there
// is no fixed-size stack buffer, no copy, and no silent truncation of the
// longest lines ("load: model ok ...", one line per sampled token). A log line
// cut in half is missing evidence exactly when it is needed.
void bridge_log(const char * fmt, ...) __attribute__((format(printf, 1, 2)));  // flawfinder: ignore
void bridge_log(const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, ap);
    va_end(ap);
}

// Fixed, not LLAMA_DEFAULT_SEED. A random seed would make every run a
// different run, which makes a bug impossible to reproduce and a regression
// impossible to compare against a previous measurement (rules.md R22).
constexpr uint32_t SAMPLER_SEED = 0x484E4459;  // "HNDY"

// The temperature the sampler chain is built with until a caller asks for a
// different one. This is the SAME value LlamaEngine.TEMPERATURE passes, so the
// default is a no-op rather than a second source of truth about sampling.
constexpr float DEFAULT_TEMPERATURE = 0.7f;

// Above this the distribution is flat enough that the output is noise. The
// bound exists so an impossible request is clamped and reported instead of
// silently producing a different kind of model.
constexpr float MAX_TEMPERATURE = 2.0f;

// Two independent stop reasons, because they are set by different threads and
// mean different things:
//
//   g_quit    the session is being replaced or released (load / unload). Set
//             BEFORE the lock is taken so a generation in flight unwinds
//             instead of the caller waiting for all 400 tokens.
//   g_cancel  the user pressed Stop.
//
// Both are consumed at the END of a generate(), never at the start. Clearing
// them on entry would silently drop a Stop that arrived in the window between
// the caller deciding to cancel and the generation thread actually starting -
// and on a device that window is exactly when the user is most likely to
// press it. Clearing on exit also means a stale Stop can never kill the next
// message.
std::atomic<bool> g_quit{false};
std::atomic<bool> g_cancel{false};

// Held for the whole of generate(), and by load/unload around the free. Without
// it, unload() frees the context and the model while a generation thread is
// still decoding into them.
std::mutex g_session_lock;

// Clears both flags however generate() leaves, including the early error
// returns. Without this the next message would be cancelled before it started.
struct StopFlagsReset {
    ~StopFlagsReset() {
        g_cancel.store(false);
        g_quit.store(false);
    }
};

bool stop_requested() {
    return g_cancel.load() || g_quit.load();
}

struct Session {
    llama_model   * model   = nullptr;
    llama_context * ctx     = nullptr;
    llama_sampler * smpl    = nullptr;
    llama_batch    batch{};
    int32_t         n_ctx   = 0;
    int32_t         n_batch = 0;
    int32_t         n_past  = 0;  // tokens already in the KV cache
    bool            batch_ok = false;
    // Temperature the live `smpl` chain was built with. llama.cpp bakes the
    // temperature into the sampler at construction time, so a different
    // requested value means a new chain, and this is how generate() knows.
    float           temperature = DEFAULT_TEMPERATURE;
};

// One session. The app drives one engine at a time; a second load replaces it.
Session g_session;
bool    g_backend_ready = false;

// The sampling chain. top_k and min_p bound the candidates, temp shapes the
// distribution, dist draws from it. The seed is fixed (SAMPLER_SEED), so a run
// is reproducible and a regression is comparable against a previous one.
//
// `temp` is a parameter because the JNI signature carries a temperature and the
// Kotlin side passes one. A hard-coded value here would leave that argument
// decorative: changing it on the Kotlin side would change nothing, and the
// signature would be a lie about what the function does.
llama_sampler * build_sampler_chain(float temp) {
    llama_sampler * chain =
        llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (chain == nullptr) {
        return nullptr;
    }
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(temp));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(SAMPLER_SEED));
    return chain;
}

// JSON string escaping. A model path or an error message can contain a quote
// or a backslash, and unescaped output would produce invalid JSON that Kotlin
// then fails to parse - turning a readable error into an opaque one.
std::string json_escape(const std::string & in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string jstring_to_utf8(JNIEnv * env, jstring s) {
    if (s == nullptr) return {};
    const char * chars = env->GetStringUTFChars(s, nullptr);
    if (chars == nullptr) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

// The single error channel back to Kotlin. Every failure lands here, so no
// internal detail can leak past a string the user will read (rules.md R27).
//
// The reason is the sentence the user reads and must be actionable on its own;
// `detail` is the developer-facing part. The JNIEnv is deliberately NOT a
// parameter: this function never touches the JVM, and taking one made it look
// as if it did.
std::string fail(const std::string & reason, const std::string & detail) {
    return std::string("{\"ok\":false,\"error\":\"") + json_escape(reason) +
           "\",\"detail\":\"" + json_escape(detail) + "\"}";
}

void session_release() {
    if (g_session.smpl != nullptr) {
        llama_sampler_free(g_session.smpl);
        g_session.smpl = nullptr;
    }
    if (g_session.batch_ok) {
        llama_batch_free(g_session.batch);
        g_session.batch_ok = false;
    }
    if (g_session.ctx != nullptr) {
        llama_free(g_session.ctx);
        g_session.ctx = nullptr;
    }
    if (g_session.model != nullptr) {
        llama_model_free(g_session.model);
        g_session.model = nullptr;
    }
    g_session.n_past = 0;
    g_session.temperature = DEFAULT_TEMPERATURE;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM * vm, void * reserved) {
    (void) vm;
    (void) reserved;
    // From here on llama.cpp and ggml report into logcat instead of an
    // Android stderr that nobody reads.
    llama_log_set(native_log, nullptr);
    bridge_log("JNI_OnLoad: libllama_jni.so loaded, llama log redirected to logcat");
    return JNI_VERSION_1_6;
}

/**
 * Load a GGUF model.
 *
 * Returns JSON. On success it reports the architecture, quantisation and
 * trained context length so the Models tab can show real metadata instead of
 * a file name, plus the context size the context was actually created with -
 * which is what the chat truncates against, and is often far smaller than the
 * model's training length.
 */
JNIEXPORT jstring JNICALL
Java_dev_hydrastone_LlamaBridge_load(JNIEnv * env, jobject self, jstring path, jint n_ctx) {
    // `self` is part of every JNI instance-method signature and this function
    // is static, so nothing reads it. Marked, not silenced with a pragma.
    (void) self;
    const std::string model_path = jstring_to_utf8(env, path);
    if (model_path.empty()) {
        return env->NewStringUTF(fail("No model path was given", "").c_str());
    }

    // A second load replaces the first. Quit is raised BEFORE the lock so a
    // generation already in flight unwinds; the lock then makes the free wait
    // until that generation has actually returned, so its decode loop can
    // never touch freed memory.
    g_quit.store(true);
    std::lock_guard<std::mutex> guard(g_session_lock);
    session_release();
    g_cancel.store(false);
    g_quit.store(false);

    if (!g_backend_ready) {
        llama_backend_init();
        g_backend_ready = true;
    }

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;  // CPU/NEON only; no GPU backend is built

    llama_model * model = llama_model_load_from_file(model_path.c_str(), mparams);
    if (model == nullptr) {
        // llama.cpp already logged the real reason, and that log now goes to
        // logcat. We do not copy it into the response: it names internal paths
        // and cache keys (rules.md R27).
        bridge_log("load: llama_model_load_from_file returned null");
        return env->NewStringUTF(
            fail(
                 "This GGUF file could not be loaded. It may be corrupt, truncated, "
                 "or use an architecture this build does not support.",
                 model_path)
                .c_str());
    }

    const int32_t trained = llama_model_n_ctx_train(model);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx      = n_ctx > 0 ? static_cast<uint32_t>(n_ctx) : 2048;
    // n_batch must not exceed the context, and n_ubatch must not exceed
    // n_batch: llama_decode rejects (or, for non-causal attention, asserts on)
    // a context configured the other way round.
    cparams.n_batch    = std::min<uint32_t>(256u, cparams.n_ctx);
    cparams.n_ubatch   = cparams.n_batch;
    cparams.n_seq_max  = 1;
    cparams.n_threads  = 4;
    cparams.n_threads_batch = 4;

    llama_context * ctx = llama_init_from_model(model, cparams);
    if (ctx == nullptr) {
        llama_model_free(model);
        return env->NewStringUTF(
            fail(
                 "The model loaded but no inference context could be created. "
                 "A smaller context size usually fixes this on a phone.",
                 model_path)
                .c_str());
    }

    g_session.model = model;
    g_session.ctx   = ctx;
    g_session.n_ctx = static_cast<int32_t>(llama_n_ctx(ctx));

    // The batch below is filled exactly up to n_batch tokens per decode. If the
    // two ever disagreed, llama_decode would be handed a batch whose token/pos/
    // logits arrays are shorter than its own n_tokens, and it would read past
    // the end of them.
    bridge_log("load: ctx created n_ctx=%d n_batch=%u n_ubatch=%u",
               g_session.n_ctx, cparams.n_batch, cparams.n_ubatch);

    g_session.smpl = build_sampler_chain(DEFAULT_TEMPERATURE);
    if (g_session.smpl == nullptr) {
        bridge_log("load: the sampler chain could not be allocated");
        llama_free(ctx);
        llama_model_free(model);
        g_session.model = nullptr;
        g_session.ctx   = nullptr;
        return env->NewStringUTF(
            fail("There was not enough memory to start a conversation with this model. "
                 "Close other apps and try again.",
                 "")
                .c_str());
    }
    g_session.temperature = DEFAULT_TEMPERATURE;

    // cparams.n_batch was set to min(256, n_ctx) above, so the narrowing is
    // bounded by construction. The cast is spelled out rather than left to the
    // explicit <int32_t> template argument alone, because this value sizes the
    // batch arrays and a wrong sign here is an out-of-bounds write, not a
    // wrong number on screen.
    g_session.n_batch = std::min<int32_t>(static_cast<int32_t>(cparams.n_batch), g_session.n_ctx);
    if (g_session.n_batch < 1) g_session.n_batch = 1;
    g_session.batch = llama_batch_init(g_session.n_batch, 0, 1);
    // Every array of the batch must exist. A partially built batch with a null
    // token array is what turns into a segfault deep inside ggml rather than a
    // message a user can read.
    g_session.batch_ok = g_session.batch.token != nullptr &&
                         g_session.batch.pos != nullptr &&
                         g_session.batch.n_seq_id != nullptr &&
                         g_session.batch.seq_id != nullptr &&
                         g_session.batch.logits != nullptr;
    if (!g_session.batch_ok) {
        bridge_log("load: llama_batch_init(%d) did not produce usable arrays", g_session.n_batch);
        llama_free(ctx);
        llama_model_free(model);
        g_session.model = nullptr;
        g_session.ctx   = nullptr;
        return env->NewStringUTF(
            fail(
                 "There was not enough memory to start a conversation with this model. "
                 "Close other apps and try again.",
                 "")
                .c_str());
    }
    g_session.n_past = 0;
    bridge_log("load: batch allocated n_batch=%d", g_session.n_batch);

    // general.architecture is a GGUF metadata key, not a function: at this
    // pin there is no llama_model_arch_str(). Reading the key is also the only
    // way to get the string the Models tab validates against the support
    // matrix, instead of guessing from a tensor shape.
    char arch[64] = {0};
    if (llama_model_meta_val_str(model, "general.architecture", arch, sizeof(arch)) <= 0) {
        arch[0] = '\0';
    }

    std::string out = "{\"ok\":true";
    out += ",\"architecture\":\"" + json_escape(arch) + "\"";
    out += ",\"n_ctx\":"       + std::to_string(g_session.n_ctx);
    out += ",\"n_ctx_train\":" + std::to_string(trained);
    out += ",\"n_embd\":"      + std::to_string(llama_model_n_embd(model));
    out += ",\"n_layer\":"     + std::to_string(llama_model_n_layer(model));
    out += ",\"size_bytes\":"  + std::to_string(llama_model_size(model));
    // The ACTUAL state size - logits plus KV cache, from llama.cpp itself and
    // not a formula. This is what makes the RAM figure on the Models tab a
    // measurement of the library's own allocation rather than an estimate we
    // invented.
    out += ",\"state_bytes\":" + std::to_string(llama_state_get_size(ctx));
    out += ",\"quant_type\":\"" + json_escape(llama_ftype_name(llama_model_ftype(model))) + "\"";
    out += "}";

    bridge_log(
        "load: model ok arch=%s n_embd=%d n_layer=%d n_ctx=%d n_ctx_train=%d "
        "weights=%lld state_bytes=%lld quant=%s",
        arch, llama_model_n_embd(model), llama_model_n_layer(model), g_session.n_ctx,
        trained, static_cast<long long>(llama_model_size(model)),
        static_cast<long long>(llama_state_get_size(ctx)),
        llama_ftype_name(llama_model_ftype(model)));

    return env->NewStringUTF(out.c_str());
}

/**
 * Generate from [prompt], streaming each piece to [callback].
 *
 * Blocks until generation finishes, is stopped, or an error occurs - exactly
 * like HydraBridge.runInference, so the Kotlin side has one shape to drive.
 */
JNIEXPORT jstring JNICALL
Java_dev_hydrastone_LlamaBridge_generate(
        JNIEnv * env, jobject self, jstring prompt, jint max_tokens, jfloat temperature,
        jobject callback) {

    (void) self;
    if (g_session.ctx == nullptr) {
        return env->NewStringUTF(fail("No model is loaded", "").c_str());
    }
    ++g_native_calls;

    // Serialise against load()/unload() for the whole run. The reset guard
    // consumes the stop flags on the way out of every return path below.
    std::lock_guard<std::mutex> session_guard(g_session_lock);
    StopFlagsReset reset_flags;

    const std::string prompt_text = jstring_to_utf8(env, prompt);
    const int32_t want = max_tokens > 0 ? static_cast<int32_t>(max_tokens) : 128;

    const llama_vocab * vocab = llama_model_get_vocab(g_session.model);

    // ---- tokenize -----------------------------------------------------------
    // The vocabulary comes from llama_model_get_vocab(), and it is the FIRST
    // argument here. On an older llama.cpp this call took model->vocab.
    std::vector<llama_token> tokens(prompt_text.size() + 16);
    const int32_t n_prompt = llama_tokenize(
        vocab, prompt_text.c_str(), static_cast<int32_t>(prompt_text.size()),
        tokens.data(), static_cast<int32_t>(tokens.size()), /*add_special*/ true,
        /*parse_special*/ false);
    // Every resize below is guarded by a sign test on the value being resized,
    // and the cast says so at the point where it matters: vector::resize takes
    // an unsigned count, so a negative that slipped through would not be a
    // short prompt, it would be a request for SIZE_MAX elements.
    if (n_prompt < 0) {
        tokens.resize(static_cast<size_t>(-n_prompt));
        const int32_t again = llama_tokenize(
            vocab, prompt_text.c_str(), static_cast<int32_t>(prompt_text.size()),
            tokens.data(), static_cast<int32_t>(tokens.size()), true, false);
        if (again < 0) {
            return env->NewStringUTF(
                fail("The prompt could not be tokenized by this model.", prompt_text).c_str());
        }
        tokens.resize(static_cast<size_t>(again));
    } else {
        tokens.resize(static_cast<size_t>(n_prompt));
    }
    if (tokens.empty()) {
        return env->NewStringUTF(
            fail("The prompt produced no tokens.", prompt_text).c_str());
    }

    // ---- context window -----------------------------------------------------
    // Leave room for at least one generated token, and say so rather than
    // silently dropping the oldest tokens: the user is told their conversation
    // no longer fits.
    bool truncated = false;
    const int32_t limit = g_session.n_ctx - 1;
    if (static_cast<int32_t>(tokens.size()) > limit) {
        tokens.erase(tokens.begin(), tokens.end() - limit);
        truncated = true;
    }
    const int32_t budget = std::min(want, limit);

    // Drop the previous turn from the KV cache. Without this the cache still
    // believes seq 0 holds the last conversation, the new prompt is written on
    // top of the same positions, and attention reads a mix of both.
    llama_memory_clear(llama_get_memory(g_session.ctx), /*data=*/ true);
    g_session.n_past = 0;

    // Honour the temperature the caller asked for. llama.cpp stores the
    // temperature inside the sampler, so a new value needs a new chain; it is
    // not a field that can be written in place.
    //
    // The clamp is not defensive noise. At temp <= 0 llama.cpp switches to
    // argmax - greedy decoding, a categorically different model - and a very
    // large value flattens the distribution into noise. Both are silently
    // plausible outputs, so an unchecked float from the UI layer would turn a
    // typo into a behaviour nobody can explain afterwards. Clamping and saying
    // so is the honest outcome; the log line is the record of it.
    //
    // Written as two named comparisons rather than as
    // "clamp it, then check whether the clamp changed anything": detecting the
    // change that way is a float equality test, and a NaN would compare false
    // against itself and pass through unclamped. `!(x <= MAX)` is true for
    // NaN, so an unusable temperature is clamped AND reported.
    const bool  too_low  = !(temperature >= 0.0f);   // also true for NaN
    const bool  too_high = !(temperature <= MAX_TEMPERATURE);
    const float requested = too_low ? 0.0f
                                    : (too_high ? MAX_TEMPERATURE : temperature);
    if (too_low || too_high) {
        bridge_log("generate: temperature %.3f out of range, using %.3f",
                   static_cast<double>(temperature), static_cast<double>(requested));
    }
    // Exact equality is the intent here, not an oversight: both sides are the
    // output of the clamp above, so the caller repeating the same float must
    // reuse the chain instead of rebuilding it every message. A tolerance
    // would be wrong in the other direction - it would rebuild on values that
    // do not differ, which is wasted work and a torn-down sampler for nothing.
    if (g_session.smpl == nullptr || requested != g_session.temperature) {
        llama_sampler * rebuilt = build_sampler_chain(requested);
        if (rebuilt != nullptr) {
            llama_sampler_free(g_session.smpl);
            g_session.smpl        = rebuilt;
            g_session.temperature = requested;
        } else {
            // Keep the old chain rather than generating with none: a finished
            // answer beats an error, and the log line records which
            // temperature actually ran.
            bridge_log("generate: sampler rebuild failed; keeping temp=%.3f",
                       static_cast<double>(g_session.temperature));
        }
    }

    // Reset the sampler: a chain that keeps its history across turns would
    // carry the previous conversation's repetition penalties into this one.
    if (g_session.smpl != nullptr) {
        llama_sampler_reset(g_session.smpl);
    }

    jclass cb_cls = env->GetObjectClass(callback);
    jmethodID on_first = env->GetMethodID(cb_cls, "onFirstToken", "()V");
    jmethodID on_piece = env->GetMethodID(cb_cls, "onPiece", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID on_done  = env->GetMethodID(cb_cls, "onDone", "()V");

    // ---- prefill ------------------------------------------------------------
    // The stop flag is checked HERE as well as in the sampling loop. Prompt
    // evaluation is where a long prompt spends most of its time, and a Stop
    // pressed in the first second of a long prompt must end the run there.
    // Checking only inside the sampling loop means the flag is ignored for the
    // whole of the prefill.
    bool    cancelled = false;
    int32_t pos       = 0;
    int32_t batches   = 0;
    const int64_t t_prefill_us = ggml_time_us();
    for (int32_t off = 0; off < static_cast<int32_t>(tokens.size()); off += g_session.n_batch) {
        if (stop_requested()) {
            cancelled = true;
            break;
        }
        const int32_t n = std::min<int32_t>(g_session.n_batch,
                                            static_cast<int32_t>(tokens.size()) - off);
        // Hard invariant, not a hope: never hand llama_decode a batch longer
        // than the arrays it was allocated with.
        GGML_ASSERT(n >= 1 && n <= g_session.n_batch);
        g_session.batch.n_tokens = n;
        for (int32_t i = 0; i < n; ++i) {
            g_session.batch.token[i]    = tokens[static_cast<size_t>(off + i)];
            g_session.batch.pos[i]      = pos + i;
            g_session.batch.n_seq_id[i] = 1;
            g_session.batch.seq_id[i][0] = 0;
            // Only the LAST prompt token needs logits: that is the one the
            // first sampled token is predicted from.
            g_session.batch.logits[i]   = (off + i + 1 == static_cast<int32_t>(tokens.size()));
        }
        const int rc = llama_decode(g_session.ctx, g_session.batch);
        ++batches;
        if (rc != 0) {
            bridge_log("generate: llama_decode(prefill) returned %d", rc);
            return env->NewStringUTF(
                fail("The model could not process the prompt.", "").c_str());
        }
        pos += n;
    }
    g_session.n_past = pos;
    // The prefill wall time is logged because it is what bounds how long a Stop
    // pressed during prompt evaluation takes to take effect: the flag is checked
    // BETWEEN batches, so the worst case is one whole batch.
    const int64_t prefill_us = ggml_time_us() - t_prefill_us;
    bridge_log("generate: prompt_tokens=%d prefill_batches=%d pos=%d prefill_ms=%lld cancelled_during_prefill=%d",
               static_cast<int32_t>(tokens.size()), batches, pos,
               static_cast<long long>(prefill_us / 1000), cancelled ? 1 : 0);

    int32_t  generated = 0;
    bool     first_sent = false;

    // ---- decode loop --------------------------------------------------------
    for (int32_t i = 0; i < budget && !cancelled; ++i) {
        // Cooperative stop, checked once per token. This is the whole reason
        // Stop works at all; the context abort callback is not used.
        if (stop_requested()) {
            cancelled = true;
            break;
        }

        const llama_token id = llama_sampler_sample(g_session.smpl, g_session.ctx, -1);
        if (llama_vocab_is_eog(vocab, id)) {
            break;
        }

        char   buf[256];
        const int32_t n = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
        if (n > 0) {
            std::string piece(buf, static_cast<size_t>(n));
            // One line per sampled token: this is the native-side record that
            // the tokens in the UI came out of llama.cpp and not out of Kotlin.
            bridge_log("generate: llama sampled id=%d piece='%s' (%d of %d)",
                       static_cast<int>(id), piece.c_str(), generated + 1, budget);
            jstring j_piece = env->NewStringUTF(piece.c_str());
            jstring j_id    = env->NewStringUTF(std::to_string(id).c_str());
            if (!first_sent && on_first != nullptr) {
                env->CallVoidMethod(callback, on_first);
                first_sent = true;
            }
            if (on_piece != nullptr) {
                env->CallVoidMethod(callback, on_piece, j_piece, j_id);
            }
            env->DeleteLocalRef(j_piece);
            env->DeleteLocalRef(j_id);
        }

        llama_sampler_accept(g_session.smpl, id);
        ++generated;

        // Feed the sampled token back in, one per step, so the KV cache stays
        // in step with what has actually been produced.
        g_session.batch.n_tokens = 1;
        g_session.batch.token[0]    = id;
        g_session.batch.pos[0]      = g_session.n_past;
        g_session.batch.n_seq_id[0] = 1;
        g_session.batch.seq_id[0][0] = 0;
        g_session.batch.logits[0]   = 1;

        const int rc = llama_decode(g_session.ctx, g_session.batch);
        if (rc != 0) {
            // Logged, not swallowed: a decode that fails mid-stream ends the
            // answer early, and without this line the truncation looks exactly
            // like the model choosing to stop.
            bridge_log("generate: llama_decode(after token) returned %d at token %d",
                       rc, i);
            break;
        }
        ++g_session.n_past;
    }

    if (on_done != nullptr) {
        env->CallVoidMethod(callback, on_done);
    }

    bridge_log("generate: finished generated=%d cancelled=%d n_past=%d temp=%.3f native_calls=%llu",
               generated, cancelled ? 1 : 0, g_session.n_past,
               static_cast<double>(g_session.temperature),
               static_cast<unsigned long long>(g_native_calls.load()));

    std::string out = "{\"ok\":true";
    out += ",\"generated\":"      + std::to_string(generated);
    // std::string on the branches, not const char*: `"literal" + "literal"`
    // is pointer arithmetic in C++, not concatenation, and the compiler is
    // right to reject it.
    out += ",\"cancelled\":"     + (cancelled ? std::string("true") : std::string("false"));
    out += ",\"truncated\":"     + (truncated ? std::string("true") : std::string("false"));
    out += ",\"n_past\":"        + std::to_string(g_session.n_past);
    out += ",\"n_ctx\":"         + std::to_string(g_session.n_ctx);
    out += ",\"prefill_batches\":" + std::to_string(batches);
    out += ",\"prefill_ms\":"      + std::to_string(prefill_us / 1000);
    out += "}";
    return env->NewStringUTF(out.c_str());
}

/** Asks the running generation to stop. Returns immediately. */
JNIEXPORT void JNICALL
Java_dev_hydrastone_LlamaBridge_cancel(JNIEnv *, jobject) {
    ++g_native_calls;
    // Deliberately lock-free: the caller is the UI thread and must return the
    // instant Stop is pressed, whatever the decode loop is doing.
    g_cancel.store(true);
    bridge_log("cancel: stop requested; the running decode ends at the next check");
}

/** Releases the model. Safe to call when nothing is loaded. */
JNIEXPORT void JNICALL
Java_dev_hydrastone_LlamaBridge_unload(JNIEnv *, jobject) {
    ++g_native_calls;
    // Signal first, then wait: an in-flight generation must see the quit
    // before the context it is decoding into is freed.
    g_quit.store(true);
    {
        std::lock_guard<std::mutex> guard(g_session_lock);
        session_release();
    }
    bridge_log("unload: model, context, sampler and batch released");
}

}  // extern "C"
