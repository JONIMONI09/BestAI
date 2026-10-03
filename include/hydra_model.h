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
} HydraEngine;

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
/* Prefill: faedt eine ganze Token-Sequenz durch die Engine, ohne sie
 * auszugeben. Danach ist der State so weit gelaufen, wie das Prompt ihn
 * getrieben hat, und hydra_engine_step() erzeugt die Fortsetzung.
 * Das ist die einzige Moeglichkeit, mehr als ein Token in die Engine zu
 * geben - hydra_engine_step() nimmt definitionsgemaess genau einen Seed.
 * n == 0 ist ein gültiges No-Op. */
int hydra_engine_prefill(HydraEngine *engine, const uint16_t *tokens, size_t n);
int hydra_verify_axiom(float humanity_factor, float proposed_score, float *safe_score);

#endif /* HYDRA_MODEL_H */
