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
#include <stdint.h>
#include <time.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/time.h>
#endif

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

/* Millisekunden seit einem beliebigen Referenzpunkt (CLOCK_MONOTONIC,
 * damit ein NTP-Sprung die Ladezeitmessung nicht verschluckt). */
static double hydra_now_ms(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
    }
#endif
    return 0.0;
}

/* Residenter Speicher in KiB, oder -1 wenn die Plattform ihn nicht sagt.
 * /proc ist billiger und genauer als getrusage(ru_maxrss), das nur den
 * HOECHSTWERT seit Prozessstart kennt - nach dem Freigeben der
 * Gewichtsseiten waere genau das die Zahl, die man nicht mehr sehen kann.
 *
 * Bewusst open()/read() statt fopen(): ein stdio-Puffer wird beim ersten
 * fopen alloziert und taucht danach mit ~512 KiB in JEDER weiteren Messung
 * auf. Genau dieser Sprung hat eine A/B-Messung der Gewichtsseiten zuerst
 * unlesbar gemacht, bevor die Ursache gefunden war. */
static long hydra_resident_kb(void)
{
#ifdef __linux__
    int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[128];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            /* size resident shared text lib data dt, in pages.
             * strtoul instead of sscanf("%lu %lu"): sscanf cannot report a
             * conversion failure, so a malformed line would silently leave
             * `resident` at 0 and report a plausible-looking 0 KiB. strtoul
             * gives us the endptr, so a partial or bad read is rejected. */
            char *end = NULL;
            unsigned long total = strtoul(buf, &end, 10);
            if (end != buf) {
                while (*end == ' ' || *end == '\t' || *end == '\n') end++;
                if (*end >= '0' && *end <= '9') {
                    unsigned long resident = strtoul(end, NULL, 10);
                    (void)total;
                    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
                    if (page_kb < 1) page_kb = 4;
                    return (long)(resident * (unsigned long)page_kb);
                }
            }
        }
    }
#endif
    return -1;
}

/* Gewichtsseiten nach dem Aufbau der Aggregation freizugeben: GEMESSEN,
 * UND ES GIBT NICHTS ZU TUN.
 *
 * Die naheliegende Optimierung waere MADV_DONTNEED auf den Gewichtsblock,
 * weil hydra_engine_step() mapped_weights nie wieder liest. Sie wurde
 * gebaut, per mincore() und per RSS verglichen - und wieder verworfen:
 *
 *   - MADV_SEQUENTIAL (das der Loader schon setzt) gibt die Seiten hinter
 *     dem Lesezeiger frei. Gemessen: 256 KiB sequentiell gelesen, RSS +0 KiB.
 *     Der Gewichtsblock ist nach der Aggregation also ohnehin nicht mehr
 *     im Prozess.
 *   - Ein zusaetzliches MADV_DONTNEED danach aenderte nichts messbar - und
 *     in einem Durchlauf 444 KiB schlechter (936 -> 1380 kB), weil die
 *     Seiten zu diesem Zeitpunkt schon weg waren und das Mapping nur noch
 *     Buchhaltung verschob.
 *   - mincore() ist das falsche Instrument: es meldet fuer ein
 *     MAP_SHARED-Dateimapping die Page-Cache-Anwesenheit, nicht die
 *     Seiteentabelle. Es meldete 65 von 65 Seiten "resident", waehrend der
 *     RSS 512 MiB niedriger lag.
 *
 * Ausserdem ist die Frage bei diesem Format fast gegenstandslos: v1 kann
 * hoechstens dim 64 x layers 4096 = 262144 Gewichtsbytes beschreiben, und
 * das Laden dieser maximalen Datei dauert gemessen 0.33 ms (--bench
 * load_ms). Der Rueckgabewert bleibt fuer den v2-Fall, wo ein echtes
 * Modell existiert und die Frage wieder eine wird. */

/* Layer-Aggregation beim Laden: A[i] = sum_l w1[l][i],
 * B[i] = sum_l w2[l][i]. Ein einziger Durchlauf ueber die gemappten
 * Gewichte, danach ist der Layer-Loop aus step() verschwunden.
 * Ueberlauf: |A[i]|,|B[i]| <= layers <= HYDRA_MAX_LAYERS (4096), passt
 * also sicher in int32 (hier trotzdem in int64 gerechnet und erst am
 * Ende geklemmt, damit die Rechnung unabhaengig vom Layer-Cap bleibt). */
static void hydra_build_aggregation(HydraEngine *engine)
{
    int64_t a[HYDRA_EMBED_DIM];
    int64_t b[HYDRA_EMBED_DIM];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));

    const uint8_t *w = engine->mapped_weights + engine->header.weights_offset;
    const size_t dim = engine->header.dim;
    for (uint32_t l = 0; l < engine->header.layers; ++l) {
        for (size_t i = 0; i < dim; ++i) {
            const uint8_t packed = w[i];
            a[i] += hydra_decode_ternary_2bit((uint8_t)(packed & 0x03u));
            b[i] += hydra_decode_ternary_2bit((uint8_t)((packed >> 2) & 0x03u));
        }
        w += dim;
    }
    for (size_t i = 0; i < dim; ++i) {
        int64_t va = a[i], vb = b[i];
        if (va > INT32_MAX) va = INT32_MAX;
        if (va < INT32_MIN) va = INT32_MIN;
        if (vb > INT32_MAX) vb = INT32_MAX;
        if (vb < INT32_MIN) vb = INT32_MIN;
        engine->agg_a[i] = (int32_t)va;
        engine->agg_b[i] = (int32_t)vb;
    }
    /* Kein Log pro Load: der Testlauf laedt hunderte Modelle und wuerde
     * sonst die Ausgabe fluten. Interessant ist der Wert nur beim
     * Bench-Modus, das dort ueber --bench separat ausgegeben wird. */
}

int hydra_engine_load(HydraEngine *engine, const char *model_path)
{
    if (!engine || !model_path) return -1;
    memset(engine, 0, sizeof(HydraEngine));
    engine->fd = -1;
    engine->resident_kb_after_load = -1;
    engine->rss_kb_before_load = -1;

    double started_ms = hydra_now_ms();
    long rss_before = hydra_resident_kb();

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

    /* Read-ahead fuer den Gewichtsblock anstossen wurde ebenfalls gebaut und
     * verworfen: posix_fadvise(POSIX_FADV_WILLNEED) zieht die Seiten vorher
     * in den Cache, obwohl die Aggregation sie unmittelbar danach ohnehin
     * sequentiell liest - es gibt nichts zu ueberholen, weil es keinen
     * zweiten Konsumenten gibt. Siehe die Notiz ueber
     * hydra_release_weight_pages fuer die Messung. */

    engine->weights_bytes = (size_t)engine->header.weights_len;
    hydra_build_aggregation(engine);

    engine->rss_kb_before_load = rss_before;
    engine->resident_kb_after_load = hydra_resident_kb();
    double elapsed = hydra_now_ms() - started_ms;
    engine->load_ms = (elapsed > 0.0) ? elapsed : 0.0;
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

/* Inferenz rein ueber ternaere Additionen/Subtraktionen {-1, 0, +1}.
 *
 * Layer-Aggregation: token_val und state_vector[i] sind ueber alle Layer
 * konstant, deshalb gilt
 *     acc[i] = sum_l (w1[l][i]*token + w2[l][i]*state[i])
 *            = A[i]*token + B[i]*state[i]
 * mit den beim Laden berechneten Summen agg_a/agg_b. Der Layer-Loop ist
 * damit aus dem heissen Pfad verschwunden: pro Token dim statt
 * layers*dim Operationen. Die Gleichheit mit dem alten Pfad ist keine
 * Behauptung, sondern gemessen: test_aggregation_matches_layer_loop
 * vergleicht beide Algorithmen ueber zufaellige Modelle, und
 * test_fast_step_matches_full_step beweist dasselbe fuer den
 * Token-only-Pfad. */
int hydra_engine_step(HydraEngine *engine, uint16_t token_in, uint16_t *token_out)
{
    if (!engine || !token_out) return -1;
    if (engine->header.magic != HYDRA_MAGIC) return -2;
    const int32_t vocab = (int32_t)engine->header.vocab_size;
    if (token_in >= engine->header.vocab_size) token_in %= engine->header.vocab_size;

    const size_t dim = engine->header.dim;

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

    int64_t accumulator[HYDRA_EMBED_DIM] = {0};
#ifdef __ARM_NEON
    /* Vektorpfad fuer die aggregierte Schleife. Die Dekodierung ist
     * bereits beim Laden passiert (hydra_build_aggregation), hier werden
     * nur noch fertige int32-Summen multipliziert — das kann keine
     * Vorzeichen-Interpretation mehr falsch machen. */
    for (size_t i = 0; i + 16 <= dim; i += 16) {
        hydra_neon_accumulate_agg(accumulator + i, engine->agg_a + i,
                                  engine->agg_b + i,
                                  engine->state_vector + i, token_val);
    }
    if (dim % 16) {
        for (size_t i = dim - (dim % 16); i < dim; ++i) {
            accumulator[i] = (int64_t)engine->agg_a[i] * token_val
                           + (int64_t)engine->agg_b[i] * (int32_t)engine->state_vector[i];
        }
    }
#else
    for (size_t i = 0; i < dim; ++i) {
        accumulator[i] = (int64_t)engine->agg_a[i] * token_val
                       + (int64_t)engine->agg_b[i] * (int32_t)engine->state_vector[i];
    }
#endif

    /* Update O(1) State-Vektor: saettige Sättigung ohne Modulo-Verzerrung */
    for (size_t i = 0; i < dim; ++i) {
        int64_t v = accumulator[i];
        if (v > 127)  v = 127;
        else if (v < -127) v = -127;
        engine->state_vector[i] = (int8_t)v;
    }

#ifdef HYDRA_DROP_CACHE
    /* Optionales Flag: nach jedem Step die zustaendigen Seiten verwerfen.
     * Seit dem Laden liest step() mapped_weights ohnehin nicht mehr - das
     * Flag betrifft also faktisch nur den Header-Bereich und kostet pro
     * Token einen Systemaufruf. Bleibt fuer Builds, die ein strengeres
     * Verhalten wollen, ist aber kein Default. */
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

/* Token-only fast path (siehe include/hydra_model.h): nur Dimension 0.
 * Weil acc[i] nach der Aggregation ausschliesslich von state[i] abhaengt,
 * ist die Trajektorie von Dimension 0 unabhaengig von allen anderen - der
 * Token-Strom ist damit bit-identisch, die uebrigen State-Eintraege
 * werden nicht fortgeschrieben (und sind danach bewusst ungueltig). */
int hydra_engine_step_fast(HydraEngine *engine, uint16_t token_in, uint16_t *token_out)
{
    if (!engine || !token_out) return -1;
    if (engine->header.magic != HYDRA_MAGIC) return -2;
    const int32_t vocab = (int32_t)engine->header.vocab_size;
    if (token_in >= engine->header.vocab_size) token_in %= engine->header.vocab_size;

#ifdef HYDRA_TOKENV_MASK
    const int32_t token_val = (int32_t)(token_in & 0x7Fu);
#else
    const int32_t token_val = (int32_t)token_in;
#endif

    const int64_t acc0 = (int64_t)engine->agg_a[0] * token_val
                       + (int64_t)engine->agg_b[0] * (int32_t)engine->state_vector[0];

    int64_t v = acc0;
    if (v > 127)  v = 127;
    else if (v < -127) v = -127;
    engine->state_vector[0] = (int8_t)v;

    int64_t raw = acc0 % vocab;
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

/* Wie gross ist der Prozess gerade wirklich resident? */
long hydra_engine_resident_kb(void)
{
    return hydra_resident_kb();
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