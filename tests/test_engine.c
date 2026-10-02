/* Hydra Unit-Tests
 *
 * Prueft: Header-Endianness, Packing-Konsistenz, Decoder, Engine-Step
 * (inkl. seeded Roundtrip-/Varianz-Pruefung), NEON-vs-Skalar-Gleichheit,
 * Format-Ablehnung (OOB-PoC, Layer-Overflow-PoC, Header-Overlap),
 * Axiom-Verifizierung inkl. NaN.
 * Build: make test  |  Ausfuehrung: ./hydra-test
 */
#include "hydra_model.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* Deterministischer LCG (statt rand()): identische Sequenz auf allen
 * Plattformen/libc-Implementierungen, damit der Test portabel bleibt. */
static uint32_t lcg_state;

static void lcg_seed(uint32_t seed) { lcg_state = seed; }
static uint8_t lcg_byte(void)
{
    lcg_state = lcg_state * 1103515245u + 12345u;
    return (uint8_t)(lcg_state >> 16);
}

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    ++tests_run; \
    if (!(cond)) { \
        ++tests_failed; \
        fprintf(stderr, "FAIL: %s (Zeile %d)\n", msg, __LINE__); \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* Temporaere Dateien: mkstemp vergibt einen unvorhersagbaren Namen und
 * legt die Datei mit 0600 an — feste /tmp-Namen sind sowohl ein
 * Kollisionsrisiko bei parallelen Laeufen als auch ein Symlink-Angriffsziel
 * (BUG-11). unlink() erst im Cleanup. */
static void tmp_path_new(char *out, size_t n)
{
    static const char tmpl[] = "/tmp/hydra_test_XXXXXX";
    if (n < sizeof(tmpl)) { fprintf(stderr, "tmp_path_new: Puffer zu klein\n"); exit(1); }
    memcpy(out, tmpl, sizeof(tmpl));
    int fd = mkstemp(out);
    if (fd < 0) { perror("mkstemp"); exit(1); }
    close(fd);
}

/* Header little-endian schreiben (docs/FORMAT.md: "<IHHIIII"), damit die
 * Tests auf jeder Host-Endianness dasselbe Modell erzeugen (BUG-10). */
static void wr_u16le(FILE *f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v & 0xFFu), (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, f);
}

static void wr_u32le(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu),
                    (uint8_t)((v >> 16) & 0xFFu), (uint8_t)((v >> 24) & 0xFFu)};
    fwrite(b, 1, 4, f);
}

/* clang-tidy bugprone-easily-swappable-parameters: die Parameter sind
 * absichtlich positional (das ist der Kern des little-endian-Formattests).
 * Jede Aufrufstelle übergibt benannte Konstanten; die Checks sind deshalb
 * hier unterdrückt, in src/ bleiben sie aktiv. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void wr_header(FILE *f, uint16_t vocab, uint32_t dim, uint32_t layers,
                      uint32_t off, uint32_t wlen)
{
    wr_u32le(f, HYDRA_MAGIC);
    wr_u16le(f, HYDRA_VERSION);
    wr_u16le(f, vocab);
    wr_u32le(f, dim);
    wr_u32le(f, layers);
    wr_u32le(f, off);
    wr_u32le(f, wlen);
}

static void test_header_size(void)
{
    /* Packing-Konsistenz C<->Python: Header muss genau 24 Bytes sein */
    CHECK(sizeof(HydraModelHeader) == 24, "Header ist exakt 24 Bytes (struct pack <IHHIIII)");
}

/* Endianness-Gegenprobe: der Loader dekodiert explizit little-endian.
 * Ein natives memcpy wuerde auf big-endian Hosts falsch liegen. */
static void test_header_endianness(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(1); }
    wr_header(f, 333, 17, 3, sizeof(HydraModelHeader), 17u * 3u);
    const uint8_t w[51] = {0};
    fwrite(w, 1, sizeof(w), f);
    fclose(f);

    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc == 0, "Load: little-endian geschriebenes Header wird akzeptiert");
    if (rc == 0) {
        CHECK(e.header.vocab_size == 333 && e.header.dim == 17 && e.header.layers == 3,
              "Header-Felder werden little-endian korrekt dekodiert");
    }
    hydra_engine_unload(&e);
    unlink(path);
}

/* Echter End-to-End-Wertetest mit handgerechneten Erwartungswerten:
 * dim=1, layers=1, token=5, state startet 0, vocab=100.
 * Korrigiertes Decoding: out = (acc mod vocab + token + 1) mod vocab
 *   w=0x01 (w1=+1,w2=0): acc = +5 -> out = (5 + 5 + 1) % 100 = 11
 *   w=0x02 (w1=-1,w2=0): acc = -5 -> out = (95 + 5 + 1) % 100 = 1
 *   w=0x00 (w1= 0,w2=0): acc =  0 -> out = (0 + 5 + 1) % 100 = 6
 * ACHTUNG: der zweite Fall lieferte previously 11 statt 1. Das war das
 * Symptom des labs()-Bug (BUG-5): labs() hat das Vorzeichen vernichtet und
 * w=+1 sowie w=-1 auf denselben Token abgebildet. Der Wert wurde bewusst
 * angepasst — siehe errors.md, Eintrag "labs() vernichtet Vorzeichen".
 */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int run_single_weight_case(uint8_t weight_byte, uint16_t token_in,
                                  uint16_t vocab, uint16_t *out)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    wr_header(f, vocab, 1u, 1u, (uint32_t)sizeof(HydraModelHeader), 1u);
    fputc(weight_byte, f);
    fclose(f);

    HydraEngine e;
    if (hydra_engine_load(&e, path) != 0) { unlink(path); return -1; }
    int rc = hydra_engine_step(&e, token_in, out);
    hydra_engine_unload(&e);
    unlink(path);
    return rc;
}

static void test_decoder_known_values(void)
{
    uint16_t out = 0;

    CHECK(run_single_weight_case(0x01, 5, 100, &out) == 0, "Decoder-Case w1=+1 laeuft");
    CHECK(out == 11, "Decoder/Token-Ableitung: w=0x01, token=5 -> 11 (handgerechnet)");

    CHECK(run_single_weight_case(0x02, 5, 100, &out) == 0, "Decoder-Case w1=-1 laeuft");
    CHECK(out == 1, "Decoder/Token-Ableitung: w=0x02, token=5 -> 1 (negatives acc symmetrisch mod-iert)");

    CHECK(run_single_weight_case(0x00, 5, 100, &out) == 0, "Decoder-Case w1=0 laeuft");
    CHECK(out == 6, "Decoder/Token-Ableitung: w=0x00, token=5 -> 6 (nur token+1)");
}

/* BUG-4: `token_in & 0x7F` kollabierte bei vocab>128 je acht Token-IDs auf
 * denselben Gewichtsbeitrag. dim=1, layers=1, w1=+1, vocab=1024:
 * acc[0] = token_val und state[0] = clamp(token_val).
 *   ohne Maske : token 128 -> token_val 128 -> state[0] = 127 (gesaettigt)
 *   mit Maske  : token 128 -> token_val 0   -> state[0] = 0
 * Damit schlaegt der Test fehl, sobald jemand die Maske wieder einbaut. */
static void test_token_mask_collisions(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(1); }
    wr_header(f, 1024u, 1u, 1u, (uint32_t)sizeof(HydraModelHeader), 1u);
    fputc(0x01, f);   /* w1 = +1, w2 = 0 */
    fclose(f);

    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc == 0, "vocab=1024-Modell laedt");
    if (rc == 0) {
        uint16_t out = 0;
        rc = hydra_engine_step(&e, 128u, &out);
        CHECK(rc == 0, "Step mit Token 128 (< vocab 1024) laeuft");
        #ifdef HYDRA_TOKENV_MASK
        /* Opt-in-Build: hier ist die Kollision beabsichtigt. */
        CHECK(e.state_vector[0] == 0,
              "HYDRA_TOKENV_MASK: Token 128 wird auf 0 maskiert (Vocab-Kollaps, gewollt)");
#else
        CHECK(e.state_vector[0] == 127,
              "Kein Vocab-Kollaps: Token 128 liefert vollen Beitrag (state=127, nicht 0)");
        CHECK(e.state_vector[0] != 0,
              "Gegenprobe: mit 0x7F-Maske waere state 0 — Test faellt bei Rueckbau");
#endif
    }
    hydra_engine_unload(&e);
    unlink(path);
}

static void test_engine_step(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(1); }
    const uint32_t dim = 16, layers = 2;
    wr_header(f, 256u, dim, layers, (uint32_t)sizeof(HydraModelHeader), dim * layers);
    for (uint32_t i = 0; i < dim * layers; ++i) {
        uint8_t byte = (uint8_t)((i % 4) == 0 ? 0x01u : ((i % 4) == 1 ? 0x02u : 0x00u));
        fputc(byte, f);
    }
    fclose(f);

    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc == 0, "Load eines gueltigen Modells");
    if (rc != 0) { unlink(path); return; }

    CHECK(e.header.dim == 16, "Header dim korrekt gelesen");
    CHECK(e.header.vocab_size == 256, "Header vocab korrekt gelesen");

    uint16_t out = 0;
    rc = hydra_engine_step(&e, 7, &out);
    CHECK(rc == 0, "Step liefert Erfolg");
    CHECK(out < 256, "Output-Token innerhalb Vocab");

    hydra_engine_unload(&e);
    unlink(path);
}

/* Echte Roundtrip-/Varianz-Pruefung:
 * - 5 verschiedene Seeds erzeugen zufaellige ternaere Gewichtsmuster
 * - je Seed: 32 Steps, jeder Output muss < vocab sein
 * - je Seed: zweiter Lauf mit frischer Engine muss identische Sequenz
 *   liefern (Reproduzierbarkeit PRO SEED)
 * - ueber Seeds hinweg muessen die Sequenzen variieren (Packing-
 *   Roundtrip und Token-Ableitung reagieren wirklich auf die Gewichte)
 */
#define VARIANCE_SEEDS 5
#define VARIANCE_STEPS 32

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void write_seeded_model(const char *path, uint32_t seed,
                               uint32_t dim, uint32_t layers, uint16_t vocab)
{
    lcg_seed(seed);
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(1); }
    wr_header(f, vocab, dim, layers, (uint32_t)sizeof(HydraModelHeader), dim * layers);
    for (uint32_t i = 0; i < dim * layers; ++i) {
        uint8_t c1 = (uint8_t)(lcg_byte() % 3u);
        uint8_t c2 = (uint8_t)(lcg_byte() % 3u);
        fputc((int)(c1 | (uint8_t)(c2 << 2)), f);
    }
    fclose(f);
}

static void test_seed_roundtrip_variance(void)
{
    static const uint32_t seeds[VARIANCE_SEEDS] = {
        1u, 42u, 1337u, 0xC0FFEEDBu, 987654321u
    };
    static uint16_t seqs[VARIANCE_SEEDS][VARIANCE_STEPS];
    char path[64];
    tmp_path_new(path, sizeof(path));
    const uint16_t vocab = 256;
    const uint32_t dim = 32, layers = 2;

    for (int s = 0; s < VARIANCE_SEEDS; ++s) {
        /* Zufaellige ternare Gewichte aus Seed erzeugen (2 Codes/Byte,
         * Codes 0/1/2 gleichverteilt, 3 ausgeschlossen -> Format-konform) */
        write_seeded_model(path, seeds[s], dim, layers, vocab);

        /* Lauf 1 */
        HydraEngine e;
        if (hydra_engine_load(&e, path) != 0) {
            CHECK(0, "Seeded-Modell laedt");
            continue;
        }
        uint16_t tok = 7;
        int all_in_vocab = 1;
        for (int t = 0; t < VARIANCE_STEPS; ++t) {
            uint16_t next = 0;
            if (hydra_engine_step(&e, tok, &next) != 0) { all_in_vocab = 0; break; }
            if (next >= vocab) all_in_vocab = 0;
            seqs[s][t] = next;
            tok = next;
        }
        hydra_engine_unload(&e);
        CHECK(all_in_vocab, "Seed-Modell: alle 32 Outputs < vocab");

        /* Lauf 2: frische Engine, gleicher Seed -> identische Sequenz */
        if (hydra_engine_load(&e, path) != 0) {
            CHECK(0, "Seeded-Modell laedt (2. Lauf)");
            continue;
        }
        int reproducible = 1;
        tok = 7;
        for (int t = 0; t < VARIANCE_STEPS; ++t) {
            uint16_t next = 0;
            hydra_engine_step(&e, tok, &next);
            if (next != seqs[s][t]) { reproducible = 0; break; }
            tok = next;
        }
        hydra_engine_unload(&e);
        CHECK(reproducible, "Seed reproduzierbar: identische Sequenz ueber Engine-Instanzen");
    }

    /* Varianz ueber Seeds: nicht alle Sequenzen duerfen identisch sein */
    int any_diff = 0;
    for (int s = 1; s < VARIANCE_SEEDS && !any_diff; ++s) {
        if (memcmp(seqs[0], seqs[s], sizeof(seqs[0])) != 0) any_diff = 1;
    }
    CHECK(any_diff, "Varianz: verschiedene Seeds erzeugen verschiedene Sequenzen");
    unlink(path);
}

#ifdef __ARM_NEON
/* Skalare Referenzimplementierung von step() — unabhaengig vom NEON-Pfad.
 * Wird im selben Build gegen die Engine gefahren (BUG-1), damit die
 * Vorzeichen-Inversion im NEON-Dekoder nicht mehr unbemerkt bleibt. */
static void ref_step_scalar(const HydraModelHeader *h, const uint8_t *w,
                            int8_t *state, uint16_t token_in, uint16_t *out)
{
    int64_t acc[HYDRA_EMBED_DIM];
    memset(acc, 0, sizeof(acc));
    const int32_t vocab = (int32_t)h->vocab_size;
    if (token_in >= h->vocab_size) token_in %= h->vocab_size;
#ifdef HYDRA_TOKENV_MASK
    const int32_t token_val = (int32_t)(token_in & 0x7Fu);
#else
    const int32_t token_val = (int32_t)token_in;
#endif
    const uint8_t *p = w;
    for (uint32_t l = 0; l < h->layers; ++l) {
        for (uint32_t i = 0; i < h->dim; ++i) {
            uint8_t c1 = (uint8_t)(p[i] & 0x03u);
            uint8_t c2 = (uint8_t)((p[i] >> 2) & 0x03u);
            int w1 = (c1 == HYDRA_TERNARY_POS) ? 1 : (c1 == HYDRA_TERNARY_NEG ? -1 : 0);
            int w2 = (c2 == HYDRA_TERNARY_POS) ? 1 : (c2 == HYDRA_TERNARY_NEG ? -1 : 0);
            acc[i] += (int64_t)(w1 * token_val + w2 * state[i]);
        }
        p += h->dim;
    }
    for (uint32_t i = 0; i < h->dim; ++i) {
        int64_t v = acc[i];
        if (v > 127) v = 127; else if (v < -127) v = -127;
        state[i] = (int8_t)v;
    }
    int64_t raw = acc[0] % vocab;
    if (raw < 0) raw += vocab;
    *out = (uint16_t)((raw + (int32_t)token_in + 1) % vocab);
}

/* Kern-Regression gegen BUG-1: NEON-Pfad und skalare Referenz muessen im
 * selben Build identische Token-Sequenzen UND State-Vektoren liefern.
 * Vor dem Fix lieferte der NEON-Pfad exakt negierte Gewichte.
 * dim=64 (Vielfaches von 16) erzwingt vollstaendige NEON-Chunks. */
static void test_neon_matches_scalar(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    const uint32_t dim = 64, layers = 3;
    const uint16_t vocab = 512;
    write_seeded_model(path, 0xDECAFBADu, dim, layers, vocab);

    /* Gewichte separat einlesen fuer die Referenz */
    uint8_t *wbytes = (uint8_t *)malloc(dim * layers);
    if (!wbytes) { perror("malloc"); exit(1); }
    FILE *rf = fopen(path, "rb");
    if (!rf) { perror("fopen"); exit(1); }
    if (fseek(rf, (long)sizeof(HydraModelHeader), SEEK_SET) != 0 ||
        fread(wbytes, 1, dim * layers, rf) != dim * layers) {
        fprintf(stderr, "Gewichte nicht lesbar\n"); exit(1);
    }
    fclose(rf);

    HydraEngine e;
    if (hydra_engine_load(&e, path) != 0) {
        CHECK(0, "NEON-Vergleichsmodell laedt");
        free(wbytes); unlink(path); return;
    }

    HydraModelHeader h = e.header;
    int8_t ref_state[HYDRA_EMBED_DIM];
    memset(ref_state, 0, sizeof(ref_state));
    uint16_t tok = 11;
    int tokens_match = 1, state_match = 1;
    for (int t = 0; t < VARIANCE_STEPS; ++t) {
        uint16_t a = 0, b = 0;
        if (hydra_engine_step(&e, tok, &a) != 0) { tokens_match = 0; break; }
        ref_step_scalar(&h, wbytes, ref_state, tok, &b);
        if (a != b) tokens_match = 0;
        if (memcmp(e.state_vector, ref_state, sizeof(ref_state)) != 0) state_match = 0;
        tok = a;
    }
    hydra_engine_unload(&e);
    free(wbytes);
    unlink(path);

    CHECK(tokens_match, "NEON-Pfad == skalare Referenz (Token-Sequenz, gleicher Build)");
    CHECK(state_match, "NEON-Pfad == skalare Referenz (State-Vektor, gleicher Build)");
}
#endif /* __ARM_NEON */

static void test_engine_rejects_garbage(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        const uint8_t junk[32] = {0xDE,0xAD,0xBE,0xEF};
        fwrite(junk, 1, sizeof(junk), f);
        fclose(f);
    }
    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc != 0, "Load mit falschem Magic wird abgelehnt");

    /* BUG-7: nach einem Ladefehler muss fd == -1 sein, sonst schliesst ein
     * nachfolgendes unload() denselben Deskriptor ein zweites Mal. */
    CHECK(e.fd == -1, "Nach Ladefehler ist fd auf -1 (kein Double-Close)");
    hydra_engine_unload(&e);
    CHECK(e.fd == -1, "unload() nach Ladefehler ist idempotent (fd bleibt -1)");
    unlink(path);
}

static void test_engine_rejects_bounds_violation(void)
{
    /* Header behauptet mehr Gewichte als die Datei hergibt */
    char path[64];
    tmp_path_new(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        wr_header(f, 64u, 64u, 4u, (uint32_t)sizeof(HydraModelHeader), 1024u);
        fclose(f);
    }
    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc != 0, "Load mit Out-of-Bounds-Weights wird abgelehnt");
    hydra_engine_unload(&e);
    unlink(path);
}

static void test_engine_rejects_inconsistent_weights_len(void)
{
    /* PoC-Regressions-Test: layers*dim > weights_len muss abgelehnt werden,
     * sonst liest step() hinter das mmap (OOB-Read). */
    char path[64];
    tmp_path_new(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        wr_header(f, 64u, 64u, 4u, (uint32_t)sizeof(HydraModelHeader), 8u);
        const uint8_t w[8] = {1,1,1,1,1,1,1,1};
        fwrite(w, 1, sizeof(w), f);
        fclose(f);
    }
    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc != 0, "Load mit layers*dim > weights_len wird abgelehnt (OOB-PoC)");
    hydra_engine_unload(&e);
    unlink(path);
}

/* PoC-Regressions-Test gegen signed Integer-Overflow (BUG-2):
 *   dim=1, vocab=128, layers=16909321, weights_len=16909321,
 *   weights_offset=24, jedes Gewichtsbyte 0x01 (w1=+1), Token 127.
 *   Pro Layer +127 -> nach 16909320 Layern 2147483640, die naechste
 *   Addition ueberschreitet INT32_MAX. Dateigroesse nur ~16,9 MB.
 * Die Datei wird vollstaendig mit echten 0x01-Bytes gefuellt, damit der
 * Test auch dann faellt, wenn jemand lediglich die layers*dim-Pruefung
 * entfernt (layers*dim == weights_len waere dann gueltig). */
static void test_engine_rejects_layer_overflow(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    const uint32_t dim = 1, layers = 16909321u;
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(1); }
    wr_header(f, 128u, dim, layers, (uint32_t)sizeof(HydraModelHeader), layers);
    {
        static const uint8_t block[65536] = {1};
        uint32_t left = layers;
        while (left) {
            uint32_t n = left > sizeof(block) ? (uint32_t)sizeof(block) : left;
            fwrite(block, 1, n, f);
            left -= n;
        }
    }
    fclose(f);

    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc == -11, "Layer-Overflow-PoC wird mit -11 abgelehnt (kein UB im Akkumulator)");
    if (rc == -11) {
        printf("PASS: PoC-Parameter (layers=%u, dim=%u) korrekt erkannt\n",
               (unsigned)layers, (unsigned)dim);
    }
    hydra_engine_unload(&e);
    unlink(path);
}

/* BUG-6: weights_offset darf nicht in den Header zeigen, sonst werden
 * Headerbytes als Gewichte ausgefuehrt. */
static void test_engine_rejects_offset_in_header(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        wr_header(f, 64u, 4u, 4u, 8u, 16u);   /* weights_offset = 8 (< 24) */
        fclose(f);
    }
    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc == -12, "weights_offset innerhalb des Headers wird abgelehnt (-12)");
    hydra_engine_unload(&e);
    unlink(path);
}

/* BUG (Server-Symlink-Audit): ein Symlink im Modellpfad zeigte auf eine
 * Datei ausserhalb des vorgesehenen Verzeichnisses; die Engine oeffnete
 * sie und die Fehlermeldung verriet sogar deren Groesse. O_NOFOLLOW muss
 * das abweisen — auch fuer Aufrufer ausserhalb der Web-Console (JNI). */
static void test_engine_rejects_symlink(void)
{
    char real[64], link[64];
    tmp_path_new(real, sizeof(real));

    int fd = open(real, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) { perror("open"); exit(1); }
    FILE *f = fdopen(fd, "wb");
    if (!f) { perror("fdopen"); close(fd); exit(1); }
    const uint32_t dim = 4, layers = 1;
    wr_header(f, 64u, dim, layers, (uint32_t)sizeof(HydraModelHeader), dim * layers);
    for (uint32_t i = 0; i < dim * layers; ++i) fputc(0x01, f);
    fclose(f);

    /* Eigener Name, aber die Datei existiert nicht — Platz fuer den Link. */
    tmp_path_new(link, sizeof(link));
    unlink(link);
    if (symlink(real, link) != 0) {
        perror("symlink");
        unlink(real);
        return;
    }

    HydraEngine e;
    int rc = hydra_engine_load(&e, link);
    CHECK(rc == -2, "Symlink im Modellpfad wird abgelehnt (O_NOFOLLOW, kein following)");
    hydra_engine_unload(&e);

    /* Kontrolle: dasselbe Modell ueber den echten Pfad laedt. */
    rc = hydra_engine_load(&e, real);
    CHECK(rc == 0, "Gegenprobe: gleiches Modell ueber den echten Pfad laedt");
    hydra_engine_unload(&e);

    unlink(real);
    unlink(link);
}

static void test_axiom(void)
{
    float score = 0.0f;
    int rc = hydra_verify_axiom(1.0f, 99.5f, &score);
    CHECK(rc == 1, "Axiom erlaubt bei humanity=1.0");
    CHECK(fabsf(score - 99.5f) < 0.01f, "Score unveraendert bei humanity=1.0");

    rc = hydra_verify_axiom(0.0f, 99.5f, &score);
    CHECK(rc == 0, "Axiom blockiert bei humanity=0.0");
    CHECK(score <= -1e8f, "Score auf -1e9 gedrueckt (nicht waehlbar)");

    rc = hydra_verify_axiom(-0.5f, 10.0f, &score);
    CHECK(rc == 0, "Axiom blockiert bei negativem humanity");

    /* BUG-9: NaN <= 0.0f ist in IEEE-754 false — ohne isfinite-Pruefung
     * wuerde ein NaN-humanity_factor das Sicherheits-Gate umgehen. */
    rc = hydra_verify_axiom(NAN, 99.5f, &score);
    CHECK(rc == 0, "Axiom blockiert bei NaN-humanity (kein Bypass)");
    CHECK(score <= -1e8f, "NaN-humanity liefert -1e9 statt eines NaN-Scores");

    rc = hydra_verify_axiom(INFINITY, 99.5f, &score);
    CHECK(rc == 0, "Axiom blockiert bei +Inf-humanity (endlichkeits-Check greift)");

    rc = hydra_verify_axiom(1.0f, INFINITY, &score);
    CHECK(rc == 0, "Axiom blockiert bei nicht-endlichem proposed_score");
    CHECK(score <= -1e8f, "nicht-endlicher proposed_score liefert -1e9 statt +Inf");

    CHECK(hydra_verify_axiom(1.0f, 1.0f, NULL) == -1, "NULL-Score-Pointer abgelehnt");
}

int main(void)
{
    printf("=== Hydra Engine Unit-Tests ===\n\n");

    test_header_size();
    test_header_endianness();
    test_decoder_known_values();
    test_token_mask_collisions();
    test_engine_step();
    test_seed_roundtrip_variance();
#ifdef __ARM_NEON
    test_neon_matches_scalar();
#else
    printf("SKIP: NEON-Vergleichstest (nur auf ARM builds mit __ARM_NEON)\n");
#endif
    test_engine_rejects_garbage();
    test_engine_rejects_bounds_violation();
    test_engine_rejects_inconsistent_weights_len();
    test_engine_rejects_layer_overflow();
    test_engine_rejects_offset_in_header();
    test_engine_rejects_symlink();
    test_axiom();

    printf("\n=== %d Tests, %d Fehler ===\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}