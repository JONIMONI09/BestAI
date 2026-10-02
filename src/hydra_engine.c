#include "hydra_model.h"
#include "hydra_neon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* Explizite Little-Endian-Dekodierung: docs/FORMAT.md definiert das
 * Headerlayout als little-endian, memcpy in native Integer wuerde auf
 * Big-Endian-Maschinen ein zufaelliges Header erzeugen (BUG-10). */
static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

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

    int rc;
    /* O_NOFOLLOW: ein Symlink im Modellpfad wuerde auf eine Datei ausserhalb
     * des vorgesehenen Verzeichnisses zeigen koennen. Der Web-Console-Pfad
     * loest Symlinks bereits ueber realpath() auf, aber die Engine-API ist
     * auch aus JNI heraus aufrufbar — die Schranke gehoert deshalb hierher. */
    engine->fd = open(model_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (engine->fd < 0) {
        perror("[Hydra] Fehler beim Oeffnen der Modelldatei");
        return -2;
    }

    struct stat sb;
    if (fstat(engine->fd, &sb) < 0) { rc = -3; goto fail; }
    engine->mapped_size = (size_t)sb.st_size;
    if (engine->mapped_size < sizeof(HydraModelHeader)) {
        fprintf(stderr, "[Hydra] Datei zu klein fuer Header (%zu Bytes)\n", engine->mapped_size);
        rc = -6; goto fail;
    }

    /* Zero-RAM Streaming: das Mapping alloziert keinen Heap-Speicher fuer
     * die Gewichte; Seiten landen im Page-Cache des Kernels. */
    engine->mapped_weights = (const uint8_t *)mmap(
        NULL, engine->mapped_size, PROT_READ, MAP_SHARED, engine->fd, 0
    );

    if (engine->mapped_weights == MAP_FAILED) {
        engine->mapped_weights = NULL;
        perror("[Hydra] mmap fehlgeschlagen");
        rc = -4; goto fail;
    }

    /* Kernel-Hinweis: sequentielles Lesen -> aggressives Read-Ahead */
    madvise((void *)engine->mapped_weights, engine->mapped_size, MADV_SEQUENTIAL);

    /* Validierung Header (explizit little-endian, s. rd_u16le/rd_u32le) */
    {
        const uint8_t *raw = engine->mapped_weights;
        engine->header.magic          = rd_u32le(raw + 0);
        engine->header.version        = rd_u16le(raw + 4);
        engine->header.vocab_size     = rd_u16le(raw + 6);
        engine->header.dim            = rd_u32le(raw + 8);
        engine->header.layers         = rd_u32le(raw + 12);
        engine->header.weights_offset = rd_u32le(raw + 16);
        engine->header.weights_len    = rd_u32le(raw + 20);
    }

    if (engine->header.magic != HYDRA_MAGIC) {
        fprintf(stderr, "[Hydra] Ungueltiges Format! Magic: 0x%08X (Erwartet: 0x%08X)\n",
                engine->header.magic, HYDRA_MAGIC);
        rc = -5; goto fail;
    }
    if (engine->header.version != HYDRA_VERSION) {
        fprintf(stderr, "[Hydra] Falsche Version: %u (Erwartet: %u)\n",
                (unsigned)engine->header.version, (unsigned)HYDRA_VERSION);
        rc = -7; goto fail;
    }
    if (engine->header.dim == 0 || engine->header.dim > HYDRA_EMBED_DIM ||
        engine->header.vocab_size == 0 || engine->header.vocab_size > HYDRA_MAX_VOCAB) {
        fprintf(stderr, "[Hydra] Header ausserhalb der Grenzen: dim=%u vocab=%u\n",
                engine->header.dim, engine->header.vocab_size);
        rc = -8; goto fail;
    }
    /* Sicherheits-Check 1: Layer-Cap. Ohne diese Grenze signed-overflowt
     * der Akkumulator in step() (PoC: layers=16909321, dim=1, alle
     * Gewichte +1, Token 127 -> 2^31 Ueberschreitung bei ~16,9 MB Datei).
     * 4096 * 254 << INT32_MAX. */
    if (engine->header.layers > HYDRA_MAX_LAYERS) {
        fprintf(stderr, "[Hydra] layers=%u ueber Limit (max %u)\n",
                engine->header.layers, (unsigned)HYDRA_MAX_LAYERS);
        rc = -11; goto fail;
    }
    /* Sicherheits-Check 2: Gewichte duerfen nicht im Header stehen. */
    if (engine->header.weights_offset < sizeof(HydraModelHeader)) {
        fprintf(stderr, "[Hydra] weights_offset=%u zeigt in den Header (< %zu)\n",
                engine->header.weights_offset, sizeof(HydraModelHeader));
        rc = -12; goto fail;
    }
    /* Sicherheits-Check 3: Gewichte muessen in die Datei passen */
    {
        uint64_t needed = (uint64_t)engine->header.weights_offset + engine->header.weights_len;
        if (needed > (uint64_t)engine->mapped_size) {
            fprintf(stderr, "[Hydra] Header verweist ausserhalb der Datei (%llu > %llu)\n",
                    (unsigned long long)needed, (unsigned long long)engine->mapped_size);
            rc = -9; goto fail;
        }
    }

    /* Sicherheits-Check 4: step() liest layers*dim Bytes — dieser Bereich muss
     * durch weights_len abgedeckt sein, sonst OOB-Read hinter dem mmap
     * (PoC-bestaetigt: crafted Header konnte bis 4 GiB hinter Dateiende lesen). */
    {
        uint64_t needed_pairs = (uint64_t)engine->header.layers * engine->header.dim;
        if (needed_pairs > (uint64_t)engine->header.weights_len) {
            fprintf(stderr, "[Hydra] layers*dim (%llu) > weights_len (%u) — Format inkonsistent\n",
                    (unsigned long long)needed_pairs, engine->header.weights_len);
            rc = -10; goto fail;
        }
    }

    /* Library-Logs ausschliesslich auf stderr, damit stdout fuer
     * maschinenlesbare Ausgaben (z.B. --json) sauber bleibt. */
    fprintf(stderr, "[Hydra] Modell erfolgreich gemappt! Layers: %u, Dim: %u, Vocab: %u\n",
            engine->header.layers, engine->header.dim, engine->header.vocab_size);
    return 0;

fail:
    /* Zentraler Cleanup: setzt fd auf -1 und mapped_weights auf NULL,
     * damit ein nachfolgendes hydra_engine_unload() keinen Double-Close
     * und kein munmap() auf MAP_FAILED erzeugt. */
    engine->fd = -1;
    hydra_engine_unload(engine);
    return rc;
}

void hydra_engine_unload(HydraEngine *engine)
{
    if (!engine) return;
    if (engine->mapped_weights) {
        munmap((void *)engine->mapped_weights, engine->mapped_size);
    }
    if (engine->fd >= 0) {
        close(engine->fd);
    }
    memset(engine, 0, sizeof(HydraEngine));
    engine->fd = -1;
}

/* Inferenz rein ueber ternaere Additionen/Subtraktionen {-1, 0, +1} */
int hydra_engine_step(HydraEngine *engine, uint16_t token_in, uint16_t *token_out)
{
    if (!engine || !token_out) return -1;
    if (engine->header.magic != HYDRA_MAGIC) return -2;
    const int32_t vocab = (int32_t)engine->header.vocab_size;
    if (token_in >= engine->header.vocab_size) token_in %= engine->header.vocab_size;

    const uint8_t *w_ptr = engine->mapped_weights + engine->header.weights_offset;
    const size_t dim = engine->header.dim;

    /* Layer-Verarbeitung on-the-fly ohne Zwischenpuffer im RAM.
     * int64-Akkumulator: mit dem Layer-Cap (HYDRA_MAX_LAYERS) waere
     * int32 ausreichend, int64 macht die Ueberlauf-Freiheit aber
     * unabhaengig von jedem einzelnen Layer-Cap. */
    int64_t accumulator[HYDRA_EMBED_DIM] = {0};

    /* Token-Beitrag: ohne Maske sind alle vocab_size Tokens unterscheidbar
     * (BUG-4: `token_in & 0x7F` kollabierte bei vocab>128 je acht Token-IDs
     * auf denselben Wert). Die Maske war Overflow-Schutz fuer den int16-NEON-
     * Pfad; der ist weiterhin sicher: |w*token| <= 1023, |w*state| <= 127,
     * Summe <= 1150 << INT16_MAX. HYDRA_TOKENV_MASK stellt das alte
     * Verhalten wieder her (vocab > 128 -> nur 128 IDs unterscheidbar). */
#ifdef HYDRA_TOKENV_MASK
    const int32_t token_val = (int32_t)(token_in & 0x7Fu);
#else
    const int32_t token_val = (int32_t)token_in;
#endif

    /* NEON-Verarbeitung in 16er-Chunks; Rest ueber skalaren Fallback.
     * Beide Pfade sind bit-identisch (gleiche Decodier-/Akkumulations-
     * semantik), verifiziert durch test_neon_matches_scalar auf ARM und
     * durch die Seed-Roundtrip-Tests auf beiden Plattformen.
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
        int64_t v = accumulator[i];
        if (v > 127)  v = 127;
        else if (v < -127) v = -127;
        engine->state_vector[i] = (int8_t)v;
    }

#ifdef HYDRA_DROP_CACHE
    /* Optionales Flag: nach jedem Step die zustaendigen Seiten verwerfen,
     * damit die Gewichte nicht im Page-Cache resident bleiben. Kostet bei
     * jedem Token einen erneuten Page-Fault je 4 KiB — Trade-off
     * zugunsten eines kleinen RSS, nicht zugunsten der Geschwindigkeit. */
    madvise((void *)engine->mapped_weights, engine->mapped_size, MADV_DONTNEED);
#endif

    /* Deterministisches Decoding fuer naechsten Token.
     * labs() hat das Vorzeichen vernichtet: w=+1 und w=-1 ergaben denselben
     * Token (BUG-5). Korrektes symmetrisches Modulo in [0, vocab):
     *   out = (acc mod vocab + token + 1) mod vocab
     * Negative Akkumulatoren werden um vocab hochgezogen, damit -1 und +1
     * unterscheidbar bleiben. */
    int64_t raw = accumulator[0] % vocab;
    if (raw < 0) raw += vocab;
    *token_out = (uint16_t)((raw + (int32_t)token_in + 1) % vocab);
    return 0;
}

int hydra_engine_prefill(HydraEngine *engine, const uint16_t *tokens, size_t n)
{
    if (!engine || (n > 0 && !tokens)) return -1;
    if (engine->header.magic != HYDRA_MAGIC) return -2;
    /* Alle Prompt-Token ausser dem letzten werden verworfen: sie treiben
     * nur den State. Der letzte Prompt-Token ist der Seed fuer die erste
     * erzeugte Position (out = f(acc + token_in)), der Aufrufer kennt ihn
     * bereits und muss ihn nicht zurueckbekommen.
     *
     * Ein Token wird also durch exakt dieselbe Kernfunktion geschickt wie
     * in einem normalen Lauf - es gibt keine zweite, abweichende
     * Prompt-Implementierung, die von step() abweichen koennte. */
    for (size_t i = 0; i + 1 < n; ++i) {
        uint16_t discarded = 0;
        if (hydra_engine_step(engine, tokens[i], &discarded) != 0) return -3;
    }
    return 0;
}

int hydra_verify_axiom(float humanity_factor, float proposed_score, float *safe_score)
{
    if (!safe_score) return -1;
    /* Koexistenz-Axiom: H(s) <= 0 => U(s,a) = -inf (nicht waehlbar).
     * isfinite zuerst: in IEEE-754 ist NaN <= 0.0f == false, ein NaN
     * humanity_factor wuerde das Gate also umgehen (BUG-9). Ebenso ein
     * nicht-endlicher proposed_score, der sonst ein NaN/Inf-Ergebnis
     * zurueckgibt. */
    if (!isfinite(humanity_factor) || humanity_factor <= 0.0f) {
        *safe_score = -1e9f;
        return 0; /* Blockiert */
    }
    if (!isfinite(proposed_score)) {
        *safe_score = -1e9f;
        return 0; /* Blockiert */
    }
    *safe_score = proposed_score * humanity_factor;
    return 1; /* Bestaetigt */
}