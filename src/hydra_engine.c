#include "hydra_model.h"
#include "hydra_neon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static int hydra_decode_ternary_2bit(uint8_t code)
{
    switch (code & 0x3u) {
    case HYDRA_TERNARY_POS: return 1;
    case HYDRA_TERNARY_NEG: return -1;
    default:                return 0; /* 0 und 11(reserved) -> 0 */
    }
}

int hydra_engine_load(HydraEngine *engine, const char *model_path)
{
    if (!engine || !model_path) return -1;
    memset(engine, 0, sizeof(HydraEngine));
    engine->fd = -1;

    engine->fd = open(model_path, O_RDONLY | O_CLOEXEC);
    if (engine->fd < 0) {
        perror("[Hydra] Fehler beim Oeffnen der Modelldatei");
        return -2;
    }

    struct stat sb;
    if (fstat(engine->fd, &sb) < 0) {
        close(engine->fd);
        return -3;
    }
    engine->mapped_size = (size_t)sb.st_size;
    if (engine->mapped_size < sizeof(HydraModelHeader)) {
        fprintf(stderr, "[Hydra] Datei zu klein fuer Header (%zu Bytes)\n", engine->mapped_size);
        close(engine->fd);
        return -6;
    }

    /* Zero-RAM Streaming: OS laedt Seiten direkt vom Flash, kein Heap verbraucht */
    engine->mapped_weights = (const uint8_t *)mmap(
        NULL, engine->mapped_size, PROT_READ, MAP_SHARED, engine->fd, 0
    );

    if (engine->mapped_weights == MAP_FAILED) {
        perror("[Hydra] mmap fehlgeschlagen");
        close(engine->fd);
        return -4;
    }

    /* Kernel-Hinweis: sequentielles Lesen -> aggressives Read-Ahead */
    madvise((void *)engine->mapped_weights, engine->mapped_size, MADV_SEQUENTIAL);

    /* Validierung Header */
    memcpy(&engine->header, engine->mapped_weights, sizeof(HydraModelHeader));
    if (engine->header.magic != HYDRA_MAGIC) {
        fprintf(stderr, "[Hydra] Ungueltiges Format! Magic: 0x%08X (Erwartet: 0x%08X)\n",
                engine->header.magic, HYDRA_MAGIC);
        hydra_engine_unload(engine);
        return -5;
    }
    if (engine->header.version != HYDRA_VERSION) {
        fprintf(stderr, "[Hydra] Falsche Version: %u (Erwartet: %u)\n",
                engine->header.version, HYDRA_VERSION);
        hydra_engine_unload(engine);
        return -7;
    }
    if (engine->header.dim == 0 || engine->header.dim > HYDRA_EMBED_DIM ||
        engine->header.vocab_size == 0 || engine->header.vocab_size > HYDRA_MAX_VOCAB) {
        fprintf(stderr, "[Hydra] Header ausserhalb der Grenzen: dim=%u vocab=%u\n",
                engine->header.dim, engine->header.vocab_size);
        hydra_engine_unload(engine);
        return -8;
    }
    /* Sicherheits-Check 1: Gewichte muessen in die Datei passen */
    {
        uint64_t needed = (uint64_t)engine->header.weights_offset + engine->header.weights_len;
        if (needed > (uint64_t)engine->mapped_size) {
            fprintf(stderr, "[Hydra] Header verweist ausserhalb der Datei (%llu > %llu)\n",
                    (unsigned long long)needed, (unsigned long long)engine->mapped_size);
            hydra_engine_unload(engine);
            return -9;
        }
    }

    /* Sicherheits-Check 2: step() liest layers*dim Bytes — dieser Bereich muss
     * durch weights_len abgedeckt sein, sonst OOB-Read hinter dem mmap
     * (PoC-bestaetigt: crafted Header konnte bis 4 GiB hinter Dateiende lesen). */
    {
        uint64_t needed_pairs = (uint64_t)engine->header.layers * engine->header.dim;
        if (needed_pairs > (uint64_t)engine->header.weights_len) {
            fprintf(stderr, "[Hydra] layers*dim (%llu) > weights_len (%u) — Format inkonsistent\n",
                    (unsigned long long)needed_pairs, engine->header.weights_len);
            hydra_engine_unload(engine);
            return -10;
        }
    }

    /* Library-Logs ausschliesslich auf stderr, damit stdout fuer
     * maschinenlesbare Ausgaben (z.B. --json) sauber bleibt. */
    fprintf(stderr, "[Hydra] Modell erfolgreich gemappt! Layers: %u, Dim: %u, Vocab: %u\n",
            engine->header.layers, engine->header.dim, engine->header.vocab_size);
    return 0;
}

void hydra_engine_unload(HydraEngine *engine)
{
    if (!engine) return;
    if (engine->mapped_weights && engine->mapped_weights != MAP_FAILED) {
        munmap((void *)engine->mapped_weights, engine->mapped_size);
    }
    if (engine->fd >= 0) {
        close(engine->fd);
    }
    memset(engine, 0, sizeof(HydraEngine));
    engine->fd = -1;
}

/* Inferenz rein ueber ternäre Additionen/Subtraktionen {-1, 0, +1} */
int hydra_engine_step(HydraEngine *engine, uint16_t token_in, uint16_t *token_out)
{
    if (!engine || !token_out) return -1;
    if (engine->header.magic != HYDRA_MAGIC) return -2;
    if (token_in >= engine->header.vocab_size) token_in %= engine->header.vocab_size;

    const uint8_t *w_ptr = engine->mapped_weights + engine->header.weights_offset;
    const size_t dim = engine->header.dim;

    /* Layer-Verarbeitung on-the-fly ohne Zwischenpuffer im RAM */
    int32_t accumulator[HYDRA_EMBED_DIM] = {0};
    /* Token auf 0..127 beschraenkt (Byte-Bereich), damit w1*token auch im
     * NEON-16-bit-Pfad ohne Overflow bleibt (|w1*token| <= 16129). */
    const int32_t token_val = (int32_t)(token_in & 0x7F);

    /* NEON-Verarbeitung in 16er-Chunks; Rest ueber skalaren Fallback.
     * Beide Pfade sind bit-identisch (gleiche Decodier-/Akkumulations-
     * semantik), verifiziert durch die Seed-Roundtrip-Tests.
     * Ohne __ARM_NEON ist neon_end=0 -> der Skalar-Loop deckt alles ab. */
#ifdef __ARM_NEON
    const size_t neon_end = dim / 16 * 16;
#else
    const size_t neon_end = 0;
#endif

    for (uint32_t l = 0; l < engine->header.layers; ++l) {
#if defined(__ARM_NEON) && defined(HYDRA_USE_NEON)
        for (size_t i = 0; i < neon_end; i += 16) {
            hydra_neon_accumulate_chunk(w_ptr + i, engine->state_vector + i,
                                        token_val, accumulator + i);
        }
#endif
        for (size_t i = neon_end; i < dim; ++i) {
            /* 2 Ternary-Weights pro Byte, little-endian bit order */
            uint8_t packed = w_ptr[i];
            int8_t w1 = (int8_t)hydra_decode_ternary_2bit((uint8_t)(packed & 0x03u));
            int8_t w2 = (int8_t)hydra_decode_ternary_2bit((uint8_t)((packed >> 2) & 0x03u));

            /* Reine Akkumulation ohne Gleitkommamultiplikation */
            accumulator[i] += w1 * token_val + w2 * (int32_t)engine->state_vector[i];
        }
        w_ptr += dim;
    }

    /* Update O(1) State-Vektor: saubere Saettigung ohne Modulo-Verzerrung */
    for (size_t i = 0; i < dim; ++i) {
        int32_t v = accumulator[i];
        if (v > 127)  v = 127;
        else if (v < -127) v = -127;
        engine->state_vector[i] = (int8_t)v;
    }

    /* Simples deterministisches Decoding fuer naechsten Token */
    *token_out = (uint16_t)((labs((long)accumulator[0]) + (uint32_t)token_in + 1u)
                            % engine->header.vocab_size);
    return 0;
}

int hydra_verify_axiom(float humanity_factor, float proposed_score, float *safe_score)
{
    if (!safe_score) return -1;
    /* Koexistenz-Axiom: H(s) <= 0 => U(s,a) = -inf (nicht waehlbar) */
    if (humanity_factor <= 0.0f) {
        *safe_score = -1e9f;
        return 0; /* Blockiert */
    }
    *safe_score = proposed_score * humanity_factor;
    return 1; /* Bestaetigt */
}
