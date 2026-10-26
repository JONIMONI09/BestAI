#ifndef HYDRA_MODEL_H
#define HYDRA_MODEL_H

#include <stdint.h>
#include <stddef.h>

#define HYDRA_MAGIC   0x48594452u /* "HYDR" */
#define HYDRA_VERSION 1

#define HYDRA_EMBED_DIM   64
#define HYDRA_MAX_VOCAB   1024
/* Layer cap: bounds the worst-case accumulator magnitude in step().
 * 4096 layers * 254 (max |w1*token| + |w2*state| with vocab<=1024)
 * = 1_040_384 < INT32_MAX, so the accumulation cannot overflow even
 * without the int64_t accumulator. The cap is the actual fix; the
 * int64_t accumulator is defence in depth. */
#define HYDRA_MAX_LAYERS  4096

/* Ternary alphabet: 00=0, 01=+1, 10=-1, 11=reserved */
#define HYDRA_TERNARY_ZERO 0u
#define HYDRA_TERNARY_POS  1u
#define HYDRA_TERNARY_NEG  2u

/* Version 3 (per-layer non-linear) design parameters. All fixed by the
 * format, not per-model options: a v3 file decodes the same everywhere.
 * See docs/FORMAT.md "Version 3" for the derivations.
 *   TF_ROPE_THETA    standard RoPE base frequency.
 *   TF_RMSNORM_EPS   RMSNorm stabiliser.
 *   TF_QUANT_GAIN    float->int8 re-quantisation gain after RMSNorm:
 *                    RMSNorm leaves the vector at RMS ~1, so x*32 keeps
 *                    the typical |x| <= 3 in a healthy int8 range while
 *                    the +-127 clamp acts as the saturation.
 *   TF_SCALE_FP      the per-layer scale u16 stores absmean * 256. */
#define HYDRA_TF_ROPE_THETA  10000.0
#define HYDRA_TF_RMSNORM_EPS 1e-5f
#define HYDRA_TF_QUANT_GAIN  32.0f
#define HYDRA_TF_SCALE_FP    256

/* Header der Binärmodell-Datei */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;          /* HYDRA_MAGIC */
    uint16_t version;        /* Versionsnummer */
    uint16_t vocab_size;     /* Vokabulargröße */
    uint32_t dim;            /* Dimension (z.B. 64) */
    uint32_t layers;         /* Anzahl Layer */
    uint32_t weights_offset; /* Offset im File für Ternary Weights */
    uint32_t weights_len;    /* Gesamtbyte-Länge */
} HydraModelHeader;
#pragma pack(pop)

/* Runtime-Kontext (Null dynamischer RAM-Zuwachs bei Inferenz) */
typedef struct {
    int fd;
    const uint8_t *mapped_weights;
    size_t mapped_size;
    HydraModelHeader header;
    int8_t state_vector[HYDRA_EMBED_DIM]; /* O(1) Zustand */

    /* Layer-Aggregation: weil token_val und state_vector[] innerhalb
     * eines step() ueber alle Layer konstant sind, gilt
     *   acc[i] = sum_l (w1[l][i]*token + w2[l][i]*state[i])
     *          = A[i]*token + B[i]*state[i]
     * Die beiden Summen werden einmalig beim Laden berechnet. Damit
     * kostet ein Token dim Multiplikationen statt layers*dim.
     * |A| <= layers <= HYDRA_MAX_LAYERS, |B| <= layers, also int32. */
    int32_t agg_a[HYDRA_EMBED_DIM];
    int32_t agg_b[HYDRA_EMBED_DIM];

    /* Lademessung (Rule: messen, bevor etwas behauptet wird).
     * load_ms               Dauer von hydra_engine_load() inkl. Aggregation
     * weights_bytes         Bytes, die die Aggregation tatsaechlich liest
     * resident_kb_after_load gemessener RSS direkt nach dem Laden, oder -1
     * rss_kb_before_load    gemessener RSS direkt davor, oder -1
     * rss_delta_kb          Differenz der beiden: die Zahl, die die Aussage
     *                       "nach dem Laden ist das Modell nicht mehr
     *                       resident" belegt oder widerlegt.
     * Gemessen am groessten vom v1-Format beschreibbaren Modell
     * (dim 64 x layers 4096 = 262144 Gewichtsbytes): load_ms 0.33 ms,
     * rss_delta_kb 0 - MADV_SEQUENTIAL gibt die Seiten hinter dem
     * Lesezeiger frei. Ein zusaetzliches MADV_DONTNEED wurde gemessen und
     * brachte nichts, siehe den Kommentar in src/hydra_engine.c. */
    double load_ms;
    size_t weights_bytes;
    long resident_kb_after_load;
    long rss_kb_before_load;

    /* RNG state for hydra_sample_token() (xorshift64*). Zero after
     * hydra_engine_load(); the sampler lazily initializes it from the
     * sampler config's rng_seed on the first draw. Greedy decoding
     * never touches it, so the deterministic baseline is unaffected. */
    uint64_t rng_state;

    /* Version 3 only: tokens consumed since load (the RoPE position).
     * Always zero on v1/v2 engines (step functions refuse the other
     * format versions, so the counter has no meaning there). */
    uint32_t step_count;
} HydraEngine;

/* Compile-time pin of the context size on 64-bit (LP64) platforms
 * (C99-portable static assert; the codebase compiles with -std=c99, so
 * C11 static_assert is not available). Measured 2026-10-26: 664 bytes
 * on x86-64 - the full per-field derivation is in docs/ARCHITECTURE.md,
 * section "Memory Model". The README once claimed 656 B, the size from
 * before rng_state existed; this assert makes that kind of layout drift
 * a build error instead of a stale doc number.
 *
 * The pin is LP64-CONDITIONAL on purpose: 32-bit ABIs (armeabi-v7a in
 * the Android NDK build) use a smaller ILP32 layout, and pinning the
 * LP64 number unconditionally broke the 32-bit NDK compile
 * (2026-10-26, caught by the android-lint NDK step on PR #56). The
 * documented size claim is a 64-bit statement. The size moved 664 ->
 * 672 when step_count was added for the version-3 RoPE position
 * (2026-10-26); ARCHITECTURE.md/README carry the updated derivation. */
#if defined(__LP64__) || defined(_LP64) || defined(__LP64)
typedef char hydra_engine_size_check[sizeof(HydraEngine) == 672 ? 1 : -1];
#endif

/* Engine-Funktionen */
int hydra_engine_load(HydraEngine *engine, const char *model_path);
void hydra_engine_unload(HydraEngine *engine);
/* Residenter Speicher in KiB, oder -1 wenn die Plattform ihn nicht meldet.
 * Nur Linux/Android; anderswo ist der Rueckgabewert -1 und das ist ein
 * "nicht gemessen", kein "null". */
long hydra_engine_resident_kb(void);
int hydra_engine_step(HydraEngine *engine, uint16_t token_in, uint16_t *token_out);
/* Token-only fast path: computes dimension 0 ONLY. Because the dimensions
 * are independent after the aggregation (acc[i] depends only on state[i]),
 * this path produces BIT-IDENTICAL token streams to the full path - the
 * state vectors of dimensions 1..dim-1 are NOT advanced and are therefore
 * STALE afterwards. That is the trick: 64x less work per token for pure
 * inference with no state display.
 *
 * CONTRACT FOR CALLERS AND UIs (this is the part that used to be implicit):
 *
 *   - After any hydra_engine_step_fast() call, ONLY state[0] is valid.
 *     state[1..dim-1] still holds the values from before the call.
 *   - A UI must therefore NOT render a full state strip from a run that
 *     used this function. Showing state[1..63] next to a token-only run
 *     displays stale numbers as if they were the current state.
 *   - If a state vector is needed, use hydra_engine_step() (which updates
 *     all dimensions) or expose state[0] alone and label it as such.
 *
 * The web console uses the full path (server.js never passes --fast), which
 * is why it may show the whole strip. The Android JNI bridge also uses the
 * full path today. tests/test_engine.c verifies the token equality AND the
 * staleness of state[1..dim-1], so this comment cannot drift away from the
 * behaviour without a test noticing. */
int hydra_engine_step_fast(HydraEngine *engine, uint16_t token_in, uint16_t *token_out);
/* Forward declaration: the full struct is defined further below in this
 * header, but the version-3 API (step_tf / sampler) references the cache
 * type before that point. */
typedef struct hydra_kv_cache hydra_kv_cache_t;
/* Version 3 forward pass: executes the per-layer non-linear ternary
 * pass for one token (cross-dimension BitLinear mat-vecs, SwiGLU +
 * RMSNorm + RoPE + KV-memory + int8 re-quantisation between every
 * layer, full-dimension logit mix at the end). Zero heap: all
 * intermediate buffers are stack-local; the weights are read straight
 * from the mmap.
 *
 * kv is a caller-initialized hydra_kv_cache_t (see hydra_kv_cache_init)
 * or NULL for no memory channel. When non-NULL, the transformed state
 * of every executed token is appended (k = v = the post-RMSNorm/RoPE
 * state) and the permanent sink-0 value is added back as a residual
 * before re-quantisation, so the first transformed state persists for
 * the whole run (streaming-LLM attention-sink pattern).
 *
 * v3 engines ONLY: returns -14 with a stderr note on a v1/v2 engine
 * (use hydra_engine_step(); a silent fallback would be a different
 * model pretending to be this one). 0 on success; *token_out is the
 * argmax of the logit mix modulo vocab. Advances step_count. */
int hydra_engine_step_tf(HydraEngine *engine, uint16_t token_in,
                         uint16_t *token_out, hydra_kv_cache_t *kv);
/* Prefill: faedt eine ganze Token-Sequenz durch die Engine, ohne sie
 * auszugeben. Danach ist der State so weit gelaufen, wie das Prompt ihn
 * getrieben hat, und hydra_engine_step() erzeugt die Fortsetzung.
 * Das ist die einzige Moeglichkeit, mehr als ein Token in die Engine zu
 * geben - hydra_engine_step() nimmt definitionsgemaess genau einen Seed.
 * n == 0 ist ein gültiges No-Op. */
int hydra_engine_prefill(HydraEngine *engine, const uint16_t *tokens, size_t n);
int hydra_verify_axiom(float humanity_factor, float proposed_score, float *safe_score);

/* ---------------------------------------------------------------------
 * Sampling (min-p + repetition penalty)
 *
 * The engine's decode is ONE accumulator (acc[0]) mapped to a token, so
 * there is no native per-vocabulary logit vector. hydra_sample_token()
 * therefore works in two documented modes:
 *
 *   1. GREEDY GUARD: temperature <= 0 OR min_p >= 1 delegates to
 *      hydra_engine_step() and returns bit-identical results. This is
 *      the path the deterministic baseline sequence runs on, and it
 *      stays bit-identical no matter what the other fields say.
 *
 *   2. SAMPLING: for every candidate token c the logit is the value of
 *      that same acc[0] the engine would compute if c were fed next,
 *      L(c) = agg_a[0] * c + agg_b[0] * state[0]. Then, in order:
 *        a) repetition penalty over the recent window — positive logits
 *           divided by the penalty, negative ones multiplied by it
 *           (the sign-preserving canonical form; repeats compound);
 *        b) numerically stable softmax with scale 1/temperature
 *           (max subtracted before exp, so no overflow for the vocab
 *           cap of 1024);
 *        c) min-p filter: drop every probability P(c) < min_p * p_max,
 *           renormalize the survivors (p_max itself always survives);
 *        d) seeded xorshift64* CDF walk over the renormalized mass.
 *      The chosen candidate then advances the real state through
 *      hydra_engine_step(chosen), so the engine's state machine is the
 *      SAME as in greedy mode — only the candidate differs.
 *
 * recent_tokens is in chronological order; the window reads the last
 * penalty_window entries. The RNG state lives in the engine, so a full
 * run is reproducible from (model, start token, sampler config).
 * ------------------------------------------------------------------- */
typedef struct hydra_sampler_config {
    float temperature;        /* <= 0: greedy guard (native step)     */
    float min_p;              /* keep P(c) >= min_p * p_max; >= 1:    */
                              /*   greedy guard                        */
    float repetition_penalty; /* 1.0 = off; > 1 punishes repeats      */
    uint16_t penalty_window;  /* recent tokens the penalty reads (0 =
                                 penalty disabled regardless)          */
    uint32_t rng_seed;        /* seed for the xorshift64* stream      */
} hydra_sampler_config_t;

/* Returns 0 on success, -1 on invalid arguments, -3 if the vocabulary
 * is too small to sample (vocab < 2), -4 if the temperature is outside
 * the numerically safe range (0 < temperature > 20). */
/* kv: version 3 only - the v3 sampling mode must advance the state
 * with the same memory channel the direct step uses, so the sampler
 * takes the same caller-owned KV cache as hydra_engine_step_tf()
 * (NULL = no memory channel). Ignored on v1/v2 engines. */
int hydra_sample_token(HydraEngine *engine, uint16_t token_in,
                       const hydra_sampler_config_t *cfg,
                       const uint16_t *recent_tokens, size_t recent_len,
                       hydra_kv_cache_t *kv,
                       uint16_t *token_out);

/* -------------------------------------------------------------------
 * M4: Transformer primitives (float, zero-heap, guarded) and the
 * streaming KV cache. These are the building blocks a real
 * transformer head needs (RMSNorm on layernorm, RoPE on q/k, SwiGLU
 * in the feed-forward) and they bridge to the ternary engine: their
 * float outputs quantize into the engine's int8 state world (see the
 * swiglu bridge test in tests/test_engine.c). Every function is
 * R14-guarded: invalid arguments or non-finite input return -1 and
 * leave the output UNMODIFIED (checked before any store).
 * ------------------------------------------------------------------- */

/* RMSNorm: out[i] = x[i] / sqrt(mean_j(x[j]^2) + eps) * w[i].
 * In-place safe (out may alias x). dim 1..4096, eps > 0.
 * Returns 0 on success, -1 on invalid arguments or any non-finite
 * x[i]/w[i] (out left unmodified). */
int hydra_rmsnorm(const float *x, const float *w, size_t dim,
                  float eps, float *out);

/* Rotary position embedding (in-place, pairs (x[2i], x[2i+1])):
 *   freq_i  = 1 / pow(theta_base, 2i / dim)
 *   angle   = pos * freq_i
 *   x'[2i]   =  x[2i]*cos(angle) - x[2i+1]*sin(angle)
 *   x'[2i+1] =  x[2i]*sin(angle) + x[2i+1]*cos(angle)
 * dim even, 2..8192; theta_base > 0 finite; pos any uint32.
 * Returns 0 on success, -1 on invalid arguments or non-finite x
 * (x left unmodified). */
int hydra_rope_inplace(float *x, size_t dim, uint32_t pos, double theta_base);

/* SwiGLU: out[i] = g[i] * sigmoid(g[i]) * u[i]  (= SiLU(g) * u).
 * dim 1..4096. Returns 0 on success, -1 on invalid arguments or any
 * non-finite g[i]/u[i] (out left unmodified). */
int hydra_swiglu(const float *g, const float *u, size_t dim, float *out);

/* Streaming KV cache: fixed capacity, zero-heap (the caller provides
 * the k/v storage), O(1) memory, O(1) per append and per lookup.
 *
 * The first num_sinks appends land in permanent SINK slots
 * (streaming-LLM attention sinks) that are never evicted. The
 * remaining capacity - num_sinks slots form a CIRCULAR WINDOW that
 * overwrites its oldest non-sink entry once full, so the resident
 * set is always: sinks (append order) + the last (capacity - sinks)
 * tokens. */
/* Defined as a plain struct (NOT a repeated typedef): the name
 * hydra_kv_cache_t is bound to this tag by the forward declaration
 * above, and C99 forbids redefining a typedef (clang
 * -Wtypedef-redefinition under -std=c99 -Werror). */
struct hydra_kv_cache {
    float *k;            /* capacity * dim floats (caller storage)  */
    float *v;            /* capacity * dim floats (caller storage)  */
    uint32_t dim;        /* key/value vector length                 */
    uint32_t capacity;   /* total slots (sinks + window)            */
    uint32_t num_sinks;  /* permanent sink slots (<= capacity)      */
    uint32_t count;      /* total appends since init                */
};

/* Initialize on caller-provided storage. num_sinks <= capacity.
 * Returns 0 on success, -1 on invalid arguments (cache left zeroed). */
int hydra_kv_cache_init(hydra_kv_cache_t *c, float *k, float *v,
                        uint32_t dim, uint32_t capacity, uint32_t num_sinks);

/* Append one token's key and value (dim floats each). O(1).
 * Returns 0 on success, -1 on invalid arguments or non-finite k/v
 * (nothing is written, count unchanged). */
int hydra_kv_cache_append(hydra_kv_cache_t *c, const float *k, const float *v);

/* Resident token count: min(count, capacity). */
uint32_t hydra_kv_cache_size(const hydra_kv_cache_t *c);

/* Copy resident token i (0 = oldest resident = first sink; then
 * window tokens oldest -> newest) into k_out/v_out (dim floats each).
 * Returns 0 on success, -1 if i >= hydra_kv_cache_size(c). */
int hydra_kv_cache_get(const hydra_kv_cache_t *c, uint32_t i,
                       float *k_out, float *v_out);

/* Copy sink i (0..num_sinks-1, in append order) into k_out/v_out.
 * Returns 0 on success, -1 if the sink is not filled yet or i is
 * out of range. */
int hydra_kv_cache_sink(const hydra_kv_cache_t *c, uint32_t i,
                        float *k_out, float *v_out);

#endif /* HYDRA_MODEL_H */
