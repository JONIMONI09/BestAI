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
} HydraEngine;

/* Engine-Funktionen */
int hydra_engine_load(HydraEngine *engine, const char *model_path);
void hydra_engine_unload(HydraEngine *engine);
int hydra_engine_step(HydraEngine *engine, uint16_t token_in, uint16_t *token_out);
int hydra_verify_axiom(float humanity_factor, float proposed_score, float *safe_score);

#endif /* HYDRA_MODEL_H */
