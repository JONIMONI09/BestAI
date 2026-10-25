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

/* Prefill-Tests: der Prompt muss wirklich durch die Engine laufen, und
 * ein Prompt muss exakt dem Lauf entsprechen, den ein Aufrufer manuell
 * ausfuehren wuerde. Ohne diese Tests koennte prefill() ein No-op sein
 * und niemand wuerde es merken - genau das war der urspruengliche Fehler
 * in der Web-UI, die nur tokens[0] an die Engine gab. */
/* Referenzimplementierung des ALTEN Algorithmus: ein Decode- und
 * Mul-Add pro (Layer, Dim), ohne jede Aggregation. Sie ist die
 * unabhaengige Referenz, gegen die die neue Engine geprueft wird - ein
 * Test, der die neue Formel mit sich selbst vergleicht, wuerde nichts
 * beweisen. */
static void ref_step_layer_loop(const HydraModelHeader *h, const uint8_t *w,
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

/* Layer-Aggregation: die verdichtete Engine muss bit-identisch zum
 * alten Layer-Loop sein - gleiche Tokens UND gleiche State-Vektoren,
 * ueber mehrere Seeds und Layerzahlen hinweg. */
static void test_aggregation_matches_layer_loop(void)
{
    static const uint32_t seeds[4] = {7u, 99u, 0xBEEFu, 2024u};
    static const uint32_t layer_counts[3] = {1u, 4u, 37u};
    char path[64];
    tmp_path_new(path, sizeof(path));

    int all_match = 1, any_ran = 0;
    for (int si = 0; si < 4 && all_match; ++si) {
        for (int li = 0; li < 3 && all_match; ++li) {
            const uint32_t dim = 16, layers = layer_counts[li];
            const uint16_t vocab = 512;
            write_seeded_model(path, seeds[si], dim, layers, vocab);

            uint8_t *wbytes = (uint8_t *)malloc((size_t)dim * layers);
            if (!wbytes) { perror("malloc"); exit(1); }
            FILE *rf = fopen(path, "rb");
            if (!rf) { perror("fopen"); exit(1); }
            if (fseek(rf, (long)sizeof(HydraModelHeader), SEEK_SET) != 0 ||
                fread(wbytes, 1, (size_t)dim * layers, rf) != (size_t)dim * layers) {
                fprintf(stderr, "weights unreadable\n"); exit(1);
            }
            fclose(rf);

            HydraEngine e;
            if (hydra_engine_load(&e, path) != 0) { all_match = 0; free(wbytes); break; }
            HydraModelHeader h = e.header;
            int8_t ref_state[HYDRA_EMBED_DIM];
            memset(ref_state, 0, sizeof(ref_state));

            uint16_t tok = (uint16_t)(3 + si);
            for (int t = 0; t < 24; ++t) {
                uint16_t a = 0, b = 0;
                if (hydra_engine_step(&e, tok, &a) != 0) { all_match = 0; break; }
                ref_step_layer_loop(&h, wbytes, ref_state, tok, &b);
                if (a != b) { all_match = 0; break; }
                if (memcmp(e.state_vector, ref_state, sizeof(ref_state)) != 0) { all_match = 0; break; }
                tok = a;
                any_ran = 1;
            }
            hydra_engine_unload(&e);
            free(wbytes);
        }
    }
    unlink(path);
    CHECK(any_ran, "Aggregations-Vergleich hat tatsaechlich Schritte ausgefuehrt");
    CHECK(all_match, "Layer-Aggregation == alter Layer-Loop (Token + State, alle Seeds/Layer)");
}

/* Token-only Fast Path: muss exakt denselben Token-Strom liefern wie der
 * volle Pfad. Das ist die Behauptung, die den 64x-Speedup trägt, also
 * wird sie gemessen und nicht angenommen. */
static void test_fast_step_matches_full_step(void)
{
    static const uint32_t seeds[3] = {11u, 0x5EEDu, 777u};
    char path[64];
    tmp_path_new(path, sizeof(path));

    int all_match = 1;
    for (int si = 0; si < 3 && all_match; ++si) {
        const uint32_t dim = 32, layers = 5;
        const uint16_t vocab = 256;
        write_seeded_model(path, seeds[si], dim, layers, vocab);

        uint16_t full[32], fast[32];
        HydraEngine e;

        if (hydra_engine_load(&e, path) != 0) { all_match = 0; break; }
        uint16_t tok = (uint16_t)(9 + si);
        for (int t = 0; t < 32; ++t) {
            if (hydra_engine_step(&e, tok, &full[t]) != 0) { all_match = 0; break; }
            tok = full[t];
        }
        hydra_engine_unload(&e);

        if (hydra_engine_load(&e, path) != 0) { all_match = 0; break; }
        tok = (uint16_t)(9 + si);
        for (int t = 0; t < 32; ++t) {
            if (hydra_engine_step_fast(&e, tok, &fast[t]) != 0) { all_match = 0; break; }
            tok = fast[t];
        }
        hydra_engine_unload(&e);

        if (memcmp(full, fast, sizeof(full)) != 0) all_match = 0;
    }
    unlink(path);
    CHECK(all_match, "Token-only Fast Path liefert bit-identische Token-Stroeme");
}

/* Token-only Fast Path: only state[0] may advance.
 *
 * The header promises that state[1..dim-1] stays STALE after a fast step,
 * and the UI rules depend on that promise: a state strip rendered from a
 * token-only run would otherwise show old numbers as if they were current.
 * A documented contract nobody measures is a comment that rots, so it is
 * measured here - and the negative control checks that the test can fail.
 */
static void test_fast_step_advances_only_dimension_zero(void)
{
    char path[64];
    const uint32_t dim = 16, layers = 3;
    const uint16_t vocab = 256;
    tmp_path_new(path, sizeof(path));
    write_seeded_model(path, 4242u, dim, layers, vocab);

    HydraEngine e;
    int ok = (hydra_engine_load(&e, path) == 0);
    int state0_changed = 0;
    int others_changed = 0;

    if (ok) {
        /* Give the engine a state that differs per dimension, so "stale"
         * is observable and "everything happens to be zero" cannot pass. */
        uint16_t tok = 7;
        for (int t = 0; t < 24 && ok; ++t) {
            uint16_t next = 0;
            if (hydra_engine_step_fast(&e, tok, &next) != 0) { ok = 0; break; }
            tok = next;
        }
        /* One more step, then look at what the fast path left behind. */
        ok = ok && hydra_engine_step_fast(&e, tok, &tok) == 0;

        int nonzero_others = 0;
        for (uint32_t i = 1; i < dim; ++i) {
            if (e.state_vector[i] != 0) nonzero_others++;
        }
        /* state_vector[] is int8_t in the engine; anything non-zero in
         * 1..dim-1 after a fast-only run means the fast path touched it. */
        if (nonzero_others != 0) others_changed = 1;
        state0_changed = (e.state_vector[0] != 0);
    }
    hydra_engine_unload(&e);
    unlink(path);

    CHECK(ok, "Fast Path laesst sich ueberhaupt laufen");
    CHECK(state0_changed, "Fast Path schreibt state[0] (sonst waere der Test blind)");
    CHECK(!others_changed,
          "Fast Path laesst state[1..dim-1] unveraendert - dokumentierte Semantik");
}

/* Negativkontrolle: die Fast-Path-Aequivalenz darf nicht trivial sein.
 * Ein Fast Path, der den Token dekrementiert statt korrekt zu rechnen,
 * muss sofort auffallen. */
static void test_fast_step_negative_control(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    const uint32_t dim = 16, layers = 3;
    const uint16_t vocab = 256;
    write_seeded_model(path, 4242u, dim, layers, vocab);

    uint16_t full[16], fast[16], broken[16];
    HydraEngine e;
    if (hydra_engine_load(&e, path) != 0) { CHECK(0, "Negativkontroll-Modell laedt"); unlink(path); return; }
    uint16_t tok = 5;
    for (int t = 0; t < 16; ++t) {
        hydra_engine_step(&e, tok, &full[t]);
        tok = full[t];
    }
    hydra_engine_unload(&e);

    if (hydra_engine_load(&e, path) != 0) { CHECK(0, "Negativkontroll-Modell laedt (2)"); unlink(path); return; }
    tok = 5;
    for (int t = 0; t < 16; ++t) {
        hydra_engine_step_fast(&e, tok, &fast[t]);
        tok = fast[t];
    }
    hydra_engine_unload(&e);

    /* Absichtlich kaputt: jeder Token um 1 verschoben. */
    for (int t = 0; t < 16; ++t) {
        broken[t] = (uint16_t)((fast[t] + 1) % vocab);
    }
    unlink(path);

    CHECK(memcmp(full, fast, sizeof(full)) == 0, "Negativkontrolle A: Fast Path == voll");
    CHECK(memcmp(full, broken, sizeof(full)) != 0,
          "Negativkontrolle B: ein verschobener Fast Path waere aufgefallen");
}

static void test_prefill_equivalence(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    const uint32_t dim = 16, layers = 2;
    const uint16_t vocab = 256;
    const uint16_t prompt[4] = {7, 9, 11, 13};
    const int steps = 8;
    uint16_t via_prefill[8], via_manual[8];
    HydraEngine e;

    write_seeded_model(path, 0xABCDEF01u, dim, layers, vocab);

    /* Weg A: prefill(prompt) + steps Generierung ab dem letzten Prompt-Token */
    if (hydra_engine_load(&e, path) != 0) { CHECK(0, "Prefill-Modell laedt"); unlink(path); return; }
    if (hydra_engine_prefill(&e, prompt, 4) != 0) { CHECK(0, "prefill() erfolgreich"); hydra_engine_unload(&e); unlink(path); return; }
    uint16_t tok = prompt[3];
    int ok = 1;
    for (int s = 0; s < steps; ++s) {
        uint16_t next = 0;
        if (hydra_engine_step(&e, tok, &next) != 0) { ok = 0; break; }
        via_prefill[s] = next;
        tok = next;
    }
    hydra_engine_unload(&e);
    CHECK(ok, "prefill() + step() erzeugt eine vollstaendige Sequenz");

    /* Weg B: von Hand - die ersten drei Prompt-Tokens als Schritte,
     * deren Ausgaben verworfen, dann ab dem vierten generieren. */
    if (hydra_engine_load(&e, path) != 0) { CHECK(0, "Prefill-Modell laedt (manuell)"); unlink(path); return; }
    for (int i = 0; i < 3; ++i) {
        uint16_t discard = 0;
        if (hydra_engine_step(&e, prompt[i], &discard) != 0) { ok = 0; break; }
    }
    tok = prompt[3];
    for (int s = 0; s < steps; ++s) {
        uint16_t next = 0;
        if (hydra_engine_step(&e, tok, &next) != 0) { ok = 0; break; }
        via_manual[s] = next;
        tok = next;
    }
    hydra_engine_unload(&e);
    CHECK(ok && memcmp(via_prefill, via_manual, sizeof(via_prefill)) == 0,
          "prefill() entspricht exakt dem manuellen Prompt-Lauf");

    /* Ein Prompt muss die Ausgabe wirklich veraendern: ein No-op
     * prefill() wuerde die Zeile oben bestehen und trotzdem falsch sein. */
    uint16_t only_last[8];
    if (hydra_engine_load(&e, path) != 0) { CHECK(0, "Prefill-Modell laedt (Seed only)"); unlink(path); return; }
    tok = prompt[3];
    for (int s = 0; s < steps; ++s) {
        uint16_t next = 0;
        hydra_engine_step(&e, tok, &next);
        only_last[s] = next;
        tok = next;
    }
    hydra_engine_unload(&e);
    CHECK(memcmp(via_prefill, only_last, sizeof(via_prefill)) != 0,
          "Prompt-Laenge aendert das Ergebnis (prefill() ist kein No-op)");

    /* n == 0 und NULL sind gueltige No-ops, kein Absturz. */
    if (hydra_engine_load(&e, path) == 0) {
        CHECK(hydra_engine_prefill(&e, NULL, 0) == 0, "prefill() mit 0 Tokens ist ein No-op");
        CHECK(hydra_engine_prefill(&e, prompt, 1) == 0, "prefill() mit 1 Token aendert den State nicht");
        CHECK(hydra_engine_prefill(NULL, prompt, 4) == -1, "prefill() mit NULL-Engine abgelehnt");
        CHECK(hydra_engine_prefill(&e, NULL, 4) == -1, "prefill() mit NULL-Tokens und n>0 abgelehnt");
        hydra_engine_unload(&e);
    }
    unlink(path);
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

/* Alle vier 2-Bit-Codes in EINEM Build: der Dekoder muss 00=0, 01=+1,
 * 10=-1 und 11=0 korrekt behandeln. Ein Seed-Modell enthaelt 11 mit
 * verschwindend kleiner Wahrscheinlichkeit, ein zufaelliger Testlauf
 * haette die reservierte Variante also nie geprueft - und genau an dieser
 * Stelle saess der urspruengliche Vorzeichenfehler (0xFF-0x00 = -1 fuer
 * Code 01). Hier ist das Byte-Muster fest vorgegeben, nicht geraten. */
static void write_code_pattern_model(const char *path, uint32_t dim,
                                     uint32_t layers, uint16_t vocab)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(1); }
    wr_header(f, vocab, dim, layers, (uint32_t)sizeof(HydraModelHeader), dim * layers);
    for (uint32_t i = 0; i < dim * layers; ++i) {
        /* Cycle through the four codes 00/01/10/11 for both weights. */
        uint8_t c1 = (uint8_t)(i % 4u);
        uint8_t c2 = (uint8_t)((i / 4u + 1u) % 4u);
        fputc((int)(c1 | (uint8_t)(c2 << 2)), f);
    }
    fclose(f);
}

static void test_neon_all_ternary_codes(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    const uint32_t dim = 64, layers = 4;   /* dim%16==0 erzwingt NEON-Chunks */
    const uint16_t vocab = 512;

    write_code_pattern_model(path, dim, layers, vocab);

    uint8_t *wbytes = (uint8_t *)malloc(dim * layers);
    if (!wbytes) { perror("malloc"); exit(1); }
    FILE *rf = fopen(path, "rb");
    if (!rf) { perror("fopen"); exit(1); }
    if (fseek(rf, (long)sizeof(HydraModelHeader), SEEK_SET) != 0 ||
        fread(wbytes, 1, dim * layers, rf) != dim * layers) {
        fprintf(stderr, "Gewichte nicht lesbar\n"); exit(1);
    }
    fclose(rf);

    /* Beweis, dass der Test wirklich alle vier Codes enthaelt - sonst
     * waere die Aussage "alle vier Codes" eine Behauptung, keine Messung. */
    int seen[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < dim * layers; ++i) {
        seen[wbytes[i] & 0x03u] = 1;
        seen[(wbytes[i] >> 2) & 0x03u] = 1;
    }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3],
          "Testmodell enthaelt alle vier 2-Bit-Codes 00/01/10/11");

    HydraEngine e;
    if (hydra_engine_load(&e, path) != 0) {
        CHECK(0, "Code-Pattern-Modell laedt");
        free(wbytes); unlink(path); return;
    }
    HydraModelHeader h = e.header;
    int8_t ref_state[HYDRA_EMBED_DIM];
    memset(ref_state, 0, sizeof(ref_state));

    /* Mehrere Seeds: jedes Token laeuft durch w1 (+/-1 * token) und w2
     * (+/-1 * state), damit beide Dekoderrichtungen und der 11-Code
     * tatsaechlich auf die Akkumulation wirken. */
    int tokens_match = 1, state_match = 1;
    static const uint16_t seeds[4] = {1u, 64u, 255u, 400u};
    for (int si = 0; si < 4 && tokens_match && state_match; ++si) {
        memset(ref_state, 0, sizeof(ref_state));
        uint16_t tok = seeds[si];
        for (int t = 0; t < 12; ++t) {
            uint16_t a = 0, b = 0;
            if (hydra_engine_step(&e, tok, &a) != 0) { tokens_match = 0; break; }
            ref_step_scalar(&h, wbytes, ref_state, tok, &b);
            if (a != b) tokens_match = 0;
            if (memcmp(e.state_vector, ref_state, sizeof(ref_state)) != 0) state_match = 0;
            tok = a;
        }
    }
    hydra_engine_unload(&e);
    free(wbytes);
    unlink(path);

    CHECK(tokens_match, "NEON == Skalar fuer alle 4 Ternary-Codes (Token-Sequenz)");
    CHECK(state_match, "NEON == Skalar fuer alle 4 Ternary-Codes (State-Vektor)");

    /* Negativkontrolle auf Decoder-Ebene: ein Dekoder, der 01 als -1
     * liest (der urspruengliche Fehler), liefert fuer einen reinen
     * Plus-Modell andere Tokens als einer, der 01 als +1 liest. Wir bauen
     * beide Modelle und vergleichen sie ueber die Engine: sind sie gleich,
     * kann der Test einen Vorzeichenfehler nicht sehen - und genau dann
     * ist er als Beweis wertlos. */
    {
        char pp[64], pn[64];
        tmp_path_new(pp, sizeof(pp));
        tmp_path_new(pn, sizeof(pn));
        uint16_t plus[4], minus[4];
        HydraEngine a, b;
        memset(&a, 0, sizeof(a)); a.fd = -1; a.mapped_weights = NULL;
        memset(&b, 0, sizeof(b)); b.fd = -1; b.mapped_weights = NULL;
        write_code_pattern_model(pp, dim, layers, vocab);
        write_code_pattern_model(pn, dim, layers, vocab);
        /* Modell 1: jedes Byte 0x55 (c1=01, c2=01) -> alle Gewichte +1.
         * Modell 2: jedes Byte 0xAA (c1=10, c2=10) -> alle Gewichte -1. */
        {
            FILE *f = fopen(pp, "r+b"); if (!f) { perror("r+b"); exit(1); }
            fseek(f, (long)sizeof(HydraModelHeader), SEEK_SET);
            for (uint32_t i = 0; i < dim * layers; ++i) fputc(0x55, f);
            fclose(f);
            f = fopen(pn, "r+b"); if (!f) { perror("r+b"); exit(1); }
            fseek(f, (long)sizeof(HydraModelHeader), SEEK_SET);
            for (uint32_t i = 0; i < dim * layers; ++i) fputc(0xAA, f);
            fclose(f);
        }
        if (hydra_engine_load(&a, pp) == 0 && hydra_engine_load(&b, pn) == 0) {
            uint16_t ta = 100, tb = 100;
            for (int s = 0; s < 4; ++s) {
                uint16_t na = 0, nb = 0;
                hydra_engine_step(&a, ta, &na);
                hydra_engine_step(&b, tb, &nb);
                plus[s] = na; minus[s] = nb;
                ta = na; tb = nb;
            }
            hydra_engine_unload(&a);
            hydra_engine_unload(&b);
            CHECK(memcmp(plus, minus, sizeof(plus)) != 0,
                  "Negativkontrolle: +1- und -1-Modell liefern verschiedene Tokens");
        } else {
            hydra_engine_unload(&a);
            hydra_engine_unload(&b);
            CHECK(0, "Negativkontroll-Modelle laden");
        }
        unlink(pp);
        unlink(pn);
    }
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

/* Version 2 (4 weights per byte, docs/FORMAT.md): positive parity test and
 * negative controls.
 *
 * Hand-computed reference (the test fails if any bit order, plane order or
 * code value is wrong):
 *   vocab=64, dim=4, layers=2, v2 payload = [0x55 0xAA 0x63 0x82]
 *   layer0: A w=+1 +1 +1 +1    B w=-1 -1 -1 -1
 *   layer1: A w= 0  0 -1 +1    B w=-1  0  0 -1
 *   A = [1,1,0,2]  B = [-2,-1,-1,-2]
 *   first step, token=5, state=0: acc = [5,5,0,10], out = (5+5+1) % 64 = 11
 * The equivalent v1 file (8 bytes, 2 weights per byte) must produce the
 * identical aggregates and the identical 40-step token stream: the two
 * versions are the same model with different packing. */
static void wr_header_version(FILE *f, uint16_t version, uint16_t vocab,
                              uint32_t dim, uint32_t layers, uint32_t off,
                              uint32_t wlen)
{
    wr_u32le(f, HYDRA_MAGIC);
    wr_u16le(f, version);
    wr_u16le(f, vocab);
    wr_u32le(f, dim);
    wr_u32le(f, layers);
    wr_u32le(f, off);
    wr_u32le(f, wlen);
}

/* Runs `steps` steps with generated-token feedback; returns 0 on success and
 * stores the generated stream in `out`. */
static int run_stream(HydraEngine *e, uint16_t seed, int steps, uint16_t *out)
{
    uint16_t tok = seed;
    for (int i = 0; i < steps; ++i) {
        if (hydra_engine_step(e, tok, &tok) != 0) return -1;
        out[i] = tok;
    }
    return 0;
}

static void test_version_two_format(void)
{
    char p2[64], p1[64];
    tmp_path_new(p2, sizeof(p2));
    tmp_path_new(p1, sizeof(p1));

    /* v2 file: 4 bytes = 2 layers x (dim/4 bytes A + dim/4 bytes B).
     * The declared weights_len is exactly the read size (layers*dim/2),
     * i.e. the minimal valid v2 file. */
    {
        FILE *f = fopen(p2, "wb");
        const uint8_t body[4] = {0x55, 0xAA, 0x63, 0x82};
        if (f) {
            wr_header_version(f, 2u, 64u, 4u, 2u, (uint32_t)sizeof(HydraModelHeader), 4u);
            fwrite(body, 1, sizeof(body), f);
            fclose(f);
        }
    }
    /* v1 file with the same logical weights: one byte per (layer, dim). */
    {
        FILE *f = fopen(p1, "wb");
        const uint8_t body[8] = {0x09, 0x09, 0x09, 0x09, 0x08, 0x00, 0x02, 0x09};
        if (f) {
            wr_header_version(f, 1u, 64u, 4u, 2u, (uint32_t)sizeof(HydraModelHeader), 8u);
            fwrite(body, 1, sizeof(body), f);
            fclose(f);
        }
    }

    HydraEngine e2, e1;
    int rc2 = hydra_engine_load(&e2, p2);
    int rc1 = hydra_engine_load(&e1, p1);
    CHECK(rc2 == 0, "Version 2 (4 weights/byte) model with minimal weights_len loads");
    CHECK(rc1 == 0, "Equivalent version 1 model loads");
    if (rc2 == 0) {
        CHECK(e2.agg_a[0] == 1 && e2.agg_a[1] == 1 && e2.agg_a[2] == 0 && e2.agg_a[3] == 2,
              "v2 aggregation A matches the hand-computed [1,1,0,2]");
        CHECK(e2.agg_b[0] == -2 && e2.agg_b[1] == -1 && e2.agg_b[2] == -1 && e2.agg_b[3] == -2,
              "v2 aggregation B matches the hand-computed [-2,-1,-1,-2]");
    }
    if (rc1 == 0 && rc2 == 0) {
        int same_agg = 1;
        for (int i = 0; i < 4; ++i) {
            if (e1.agg_a[i] != e2.agg_a[i] || e1.agg_b[i] != e2.agg_b[i]) same_agg = 0;
        }
        CHECK(same_agg, "v1 and v2 aggregates are identical (same model, different packing)");
        uint16_t s1[40], s2[40];
        int ok1 = run_stream(&e1, 5u, 40, s1) == 0;
        int ok2 = run_stream(&e2, 5u, 40, s2) == 0;
        CHECK(ok1 && ok2, "both versions run 40 steps without error");
        if (ok1 && ok2) {
            int same = 1;
            for (int i = 0; i < 40; ++i) if (s1[i] != s2[i]) { same = 0; break; }
            CHECK(same, "v1 and v2 produce the identical 40-token stream");
            CHECK(s2[0] == 11, "v2 first token is 11 (hand-computed from A, B, token 5)");
        }
    }
    hydra_engine_unload(&e2);
    hydra_engine_unload(&e1);
    unlink(p2);
    unlink(p1);

    /* Negative: v2 with dim % 4 != 0 must be refused (-8), not truncated.
     * Without the guard, this file would load and silently zero out the
     * last dimension of the aggregation. */
    {
        char p[64];
        tmp_path_new(p, sizeof(p));
        FILE *f = fopen(p, "wb");
        if (f) {
            wr_header_version(f, 2u, 64u, 2u, 1u, (uint32_t)sizeof(HydraModelHeader), 1u);
            fputc(0x55, f);
            fclose(f);
        }
        HydraEngine e;
        int rc = hydra_engine_load(&e, p);
        CHECK(rc == -8, "v2 model with dim % 4 != 0 is refused with -8");
        hydra_engine_unload(&e);
        unlink(p);
    }
    /* Negative: v2 with weights_len below layers*dim/2 must be refused (-10):
     * the aggregation walk would read past the declared weight region. */
    {
        char p[64];
        tmp_path_new(p, sizeof(p));
        FILE *f = fopen(p, "wb");
        if (f) {
            wr_header_version(f, 2u, 64u, 4u, 2u, (uint32_t)sizeof(HydraModelHeader), 3u);
            const uint8_t body[3] = {0x55, 0xAA, 0x63};
            fwrite(body, 1, sizeof(body), f);
            fclose(f);
        }
        HydraEngine e;
        int rc = hydra_engine_load(&e, p);
        CHECK(rc == -10, "v2 model with weights_len < layers*dim/2 is refused with -10");
        hydra_engine_unload(&e);
        unlink(p);
    }
}

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

/* BUG (server symlink audit): a symlink in the model path pointed at a
 * file outside the intended directory; the engine opened it and the
 * error message even revealed its size. O_NOFOLLOW must reject that —
 * also for callers outside the web console (JNI). */
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

/* Lademessung: die Aussage "Modelle laden schnell" ist eine Zahl, und
 * eine Zahl veraltet. Dieser Test pinnt die Felder fest, damit niemand
 * load_ms entfernen kann, ohne dass es auffaellt - und meldet den Wert,
 * damit eine Aenderung sichtbar wird.
 *
 * Er behauptet NICHT, dass das Laden schnell ist: eine Zeitgrenze waere auf
 * einer ausgelasteten CI-Maschine flaky. Er behauptet, dass gemessen wird. */
static void test_load_metrics_are_measured(void)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    const uint32_t dim = 64, layers = 512;
    write_seeded_model(path, 31337u, dim, layers, 512);

    long warm = hydra_engine_resident_kb();
    (void)warm;   /* erster Aufruf kann den stdio-/locale-Zustand aufsetzen */

    HydraEngine e;
    CHECK(hydra_engine_load(&e, path) == 0, "Modell laedt fuer die Lademessung");
    HydraModelHeader h = e.header;
    size_t expect = (size_t)h.layers * (size_t)h.dim;

    CHECK(e.weights_bytes >= expect,
          "weights_bytes deckt layers x dim ab");
    CHECK(e.load_ms >= 0.0, "load_ms wurde gesetzt (nicht -1, nicht uninitialisiert)");
    CHECK(e.load_ms < 10000.0,
          "load_ms ist eine Zeit in Millisekunden und keine Overflow-Summe");

    /* RSS ist nur auf Linux messbar. Wo er messbar ist, muss er eine
     * plausible Groesse sein - ein 0 oder ein Rauschen waere ein Messfehler,
     * kein Messergebnis, und wird als solcher gemeldet. */
    CHECK(e.rss_kb_before_load == -1 || e.rss_kb_before_load > 0,
          "rss_kb_before_load ist entweder -1 (nicht messbar) oder positiv");
    CHECK(e.resident_kb_after_load == -1 || e.resident_kb_after_load > 0,
          "resident_kb_after_load ist entweder -1 oder positiv");

    uint16_t tok = 5, out = 0;
    CHECK(hydra_engine_step(&e, tok, &out) == 0,
          "nach der Lademessung ist das Modell weiter benutzbar");
    hydra_engine_unload(&e);
    unlink(path);
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

/* ---------------------------------------------------------------------
 * Sampler (min-p + repetition penalty) — tests for hydra_sample_token().
 *
 * The engine has no native per-vocabulary logits; the sampler probes
 * L(c) = agg_a[0]*c + agg_b[0]*state[0] (the acc[0] the engine would
 * compute if c were fed). Every model below is built so that the
 * chosen candidate c can be RECOVERED from the emitted token:
 *   out = (acc0 mod V + c + 1) mod V  with  acc0 = A0*c + B0*s0
 * which is injective in c for the (A0, B0, s0, V) combinations used
 * here — so an assertion on the output token is an assertion on the
 * sampled candidate. All expected values were hand-computed
 * (reference implementation: Python double-precision softmax + the
 * identical xorshift64* stream), not fitted to the engine's output.
 * ------------------------------------------------------------------- */

/* Build a one-dimensional v1 model (dim=1) with L layers of w1=+1,
 * w2=0 (byte 0x01): agg_a[0] = L, agg_b[0] = 0, L(c) = L*c. */
static int make_v1_model(const char *path, uint16_t vocab, uint32_t layers,
                         uint8_t weight_byte)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    wr_header(f, vocab, 1u, layers, (uint32_t)sizeof(HydraModelHeader), layers);
    for (uint32_t l = 0; l < layers; ++l) fputc(weight_byte, f);
    fclose(f);
    return 0;
}

static const uint16_t two_by_63[2] = {63, 63};

/* One-shot sample on a freshly loaded model (state starts at zero),
 * token_in = 0 (the probe does not depend on it). */
static int sample_once(uint16_t vocab, uint32_t layers, uint8_t wbyte,
                       const hydra_sampler_config_t *cfg,
                       const uint16_t *recent, size_t nrec, uint16_t *out)
{
    char path[64];
    tmp_path_new(path, sizeof(path));
    if (make_v1_model(path, vocab, layers, wbyte) != 0) { unlink(path); return -1; }
    HydraEngine e;
    if (hydra_engine_load(&e, path) != 0) { unlink(path); return -2; }
    int rc = hydra_sample_token(&e, 0, cfg, recent, nrec, out);
    hydra_engine_unload(&e);
    unlink(path);
    return rc;
}

static void test_sampler_greedy_guard(void)
{
    /* temperature <= 0 and min_p >= 1 must produce BIT-IDENTICAL
     * sequences to the native hydra_engine_step() — that is what keeps
     * the deterministic baseline sequence valid after sampling exists. */
    char path[64];
    tmp_path_new(path, sizeof(path));
    CHECK(make_v1_model(path, 64, 1, 0x01) == 0, "Sampler: Testmodell (V=64) wird geschrieben");

    HydraEngine a, b, c;
    CHECK(hydra_engine_load(&a, path) == 0, "Sampler: Modell laedt (Engine A)");
    CHECK(hydra_engine_load(&b, path) == 0, "Sampler: Modell laedt (Engine B, temp=0)");
    CHECK(hydra_engine_load(&c, path) == 0, "Sampler: Modell laedt (Engine C, min_p=1)");

    hydra_sampler_config_t cfg0, cfg1;
    cfg0.temperature = 0.0f; cfg0.min_p = 0.0f;
    cfg0.repetition_penalty = 1.0f; cfg0.penalty_window = 8; cfg0.rng_seed = 99;
    cfg1 = cfg0; cfg1.temperature = 1.0f; cfg1.min_p = 1.0f;

    int same0 = 1, same1 = 1;
    uint16_t ta = 5, tb = 5, tc = 5;
    for (int s = 0; s < 32; ++s) {
        uint16_t oa, ob, oc;
        if (hydra_engine_step(&a, ta, &oa) != 0 ||
            hydra_sample_token(&b, tb, &cfg0, &tb, (size_t)(s > 0 ? 1 : 0), &ob) != 0 ||
            hydra_sample_token(&c, tc, &cfg1, &tc, (size_t)(s > 0 ? 1 : 0), &oc) != 0) {
            same0 = same1 = 0;
            break;
        }
        if (oa != ob) same0 = 0;
        if (oa != oc) same1 = 0;
        ta = oa; tb = ob; tc = oc;
    }
    CHECK(same0, "Sampler: temp=0 (greedy guard) bit-identisch zum nativen step (32 Steps)");
    CHECK(same1, "Sampler: min_p=1 (greedy guard) bit-identisch zum nativen step (32 Steps)");
    hydra_engine_unload(&a);
    hydra_engine_unload(&b);
    hydra_engine_unload(&c);
    unlink(path);
}

static void test_sampler_repetition_penalty(void)
{
    /* Model V=64, w1=+1: L(c) = c, argmax at 63. Window = {63, 63}
     * (the token that keeps being emitted — the repetition loop).
     * At T=0.001 the softmax collapses onto the argmax with weight
     * exactly 1.0 (exp(-1030) underflows), so each one-shot sample is
     * deterministic and isolates the penalty's effect:
     *   pen=1.0: L(63)=63 stays the max        -> c=63 -> out 63
     *   pen=4.0: L(63)=63/4/4=3.9375 < L(62)=62 -> c=62 -> out 61
     * i.e. the penalty moves the emitted token off the repeated one. */
    hydra_sampler_config_t cfg;
    cfg.temperature = 0.001f; cfg.min_p = 0.0f;
    cfg.penalty_window = 2; cfg.rng_seed = 1;

    uint16_t out = 0;
    cfg.repetition_penalty = 1.0f;
    CHECK(sample_once(64, 1, 0x01, &cfg, two_by_63, 2, &out) == 0,
          "Sampler: Penalty-Testmodell (V=64) laeuft ohne Penalty");
    CHECK(out == 63, "Sampler: ohne Penalty wird der wiederholte Token 63 ausgespielt");

    cfg.repetition_penalty = 4.0f;
    CHECK(sample_once(64, 1, 0x01, &cfg, two_by_63, 2, &out) == 0,
          "Sampler: Penalty-Testmodell (V=64) laeuft mit Penalty 4.0");
    CHECK(out == 61, "Sampler: Penalty bricht die Wiederholung (63 -> 62, out 63 -> 61)");

    /* Negative-logit branch: model V=8, w1=+1, w2=-1 (A0=1, B0=-1).
     * Prefill [7,0,7,0]: prefill processes all but the last token and
     * step() mods token_in by vocab, so the state recurrence
     * s' = t - s runs 7 -> 7 -> -7 -> 14. L(c) = c - 14 =
     * [-14..-7], argmax c=7 with a NEGATIVE logit (-7). Pen(4)
     * MULTIPLIES it to -28, moving the argmax to c=6:
     *   pen=1.0: c=7 -> out 1
     *   pen=4.0: c=6 -> out 7
     * (At T=0.001 both softmaxes collapse exactly onto the argmax:
     * the second-best weight is exp(-1000) = 0.0 in double.) */
    char path[64];
    tmp_path_new(path, sizeof(path));
    CHECK(make_v1_model(path, 8, 1, 0x09) == 0, "Sampler: Negativ-Testmodell (V=8) wird geschrieben");
    HydraEngine e;
    const uint16_t pre[4] = {7, 0, 7, 0};
    int ok = hydra_engine_load(&e, path) == 0;
    if (ok) ok = hydra_engine_prefill(&e, pre, 4) == 0;
    CHECK(ok, "Sampler: Negativ-Testmodell laedt und prefillt (state=14)");
    if (ok) {
        cfg.repetition_penalty = 1.0f;
        uint16_t recent0 = 7;
        uint16_t o = 0;
        ok = hydra_sample_token(&e, 0, &cfg, &recent0, 1, &o) == 0 && o == 1;
        CHECK(ok, "Sampler: negatives Logit ohne Penalty -> c=7 (out 1)");
        hydra_engine_unload(&e);
        ok = hydra_engine_load(&e, path) == 0;
        if (ok) ok = hydra_engine_prefill(&e, pre, 4) == 0;
        if (ok) {
            cfg.repetition_penalty = 4.0f;
            uint16_t o2 = 0;
            ok = hydra_sample_token(&e, 0, &cfg, &recent0, 1, &o2) == 0 && o2 == 7;
            CHECK(ok, "Sampler: negatives Logit WIRD mit Penalty multipliziert (c=7 -> 6, out 1 -> 7)");
            hydra_engine_unload(&e);
        } else {
            CHECK(0, "Sampler: Reload fuer Negativ-Beriff fehlgeschlagen");
        }
    }
    unlink(path);
}

static void test_sampler_min_p(void)
{
    /* Model V=65, w1=+1: L(c) = c, p(c) proportional to e^c, p_max at
     * c=64. Hand-computed surviving sets (threshold min_p * p_max):
     *   min_p=0.1 -> keep {62, 63, 64}  (e^(c-64) >= 0.1  <=>  c >= 61.7)
     *   min_p=0.5 -> keep {64}          (e^(c-64) >= 0.5  <=>  c >= 63.3)
     * With out = (2c+1) mod 65 (injective for odd V):
     *   keep {62,63,64} -> out {60, 62, 64},  keep {64} -> out 64. */
    hydra_sampler_config_t cfg;
    cfg.temperature = 1.0f; cfg.min_p = 0.5f;
    cfg.repetition_penalty = 1.0f; cfg.penalty_window = 0; cfg.rng_seed = 0;

    int all64 = 1;
    for (uint32_t s = 1; s <= 16; ++s) {
        uint16_t out = 0;
        cfg.rng_seed = s;
        if (sample_once(65, 1, 0x01, &cfg, NULL, 0, &out) != 0 || out != 64) all64 = 0;
    }
    CHECK(all64, "Sampler: min_p=0.5 filtert alles bis auf p_max (16 Seeds -> immer 64)");

    cfg.min_p = 0.1f;
    int in_set = 1, mask = 0;
    for (uint32_t s = 1; s <= 24; ++s) {
        uint16_t out = 999;
        cfg.rng_seed = s;
        if (sample_once(65, 1, 0x01, &cfg, NULL, 0, &out) != 0) { in_set = 0; continue; }
        if (out != 60 && out != 62 && out != 64) in_set = 0;
        if (out == 60) mask |= 1;
        if (out == 62) mask |= 2;
        if (out == 64) mask |= 4;
    }
    CHECK(in_set, "Sampler: min_p=0.1 behaelt genau {62,63,64} (24 Seeds -> out nur 60/62/64)");
    const int n_seen = ((mask & 1) != 0) + ((mask & 2) != 0) + ((mask & 4) != 0);
    CHECK(n_seen >= 2, "Sampler: min_p=0.1 sampelt innerhalb des ueberlebenden Sets (>=2 Werte)");

    /* Control: without the filter the tail IS reachable — at least one
     * of 96 fixed seeds must land outside {60,62,64}. */
    cfg.min_p = 0.0f;
    int tail_seen = 0;
    for (uint32_t s = 1; s <= 96; ++s) {
        uint16_t out = 999;
        cfg.rng_seed = s;
        if (sample_once(65, 1, 0x01, &cfg, NULL, 0, &out) == 0 &&
            out != 60 && out != 62 && out != 64) tail_seen = 1;
    }
    CHECK(tail_seen, "Sampler: ohne min_p ist der schwache Schwanz erreichbar (96 Seeds)");
}

static void test_sampler_rng_hand_computed(void)
{
    /* Model V=5, w1=+1: L(c) = c, p proportional to e^c:
     *   p = [0.011656, 0.031685, 0.086129, 0.234121, 0.636409]
     *   CDF = [0.011656, 0.043341, 0.129470, 0.363591, 1.0]
     * xorshift64* draws (independent reference implementation):
     *   seed 1   -> u=0.280835 -> c=3 -> out (2c+1) mod 5 = 2
     *   seed 2   -> u=0.561670 -> c=4 -> out 4
     *   seed 555 -> u=0.044097 -> c=2 -> out 0
     *   seed 0   -> magic state -> u=0.052791 -> c=2 -> out 0
     * These pin the exact RNG stream AND the CDF walk. */
    hydra_sampler_config_t cfg;
    cfg.temperature = 1.0f; cfg.min_p = 0.0f;
    cfg.repetition_penalty = 1.0f; cfg.penalty_window = 0;

    uint16_t out = 0;
    cfg.rng_seed = 1;
    CHECK(sample_once(5, 1, 0x01, &cfg, NULL, 0, &out) == 0 && out == 2,
          "Sampler: Seed 1 -> xorshift64* u=0.280835 -> c=3 -> out 2 (handgerechnet)");
    cfg.rng_seed = 2;
    CHECK(sample_once(5, 1, 0x01, &cfg, NULL, 0, &out) == 0 && out == 4,
          "Sampler: Seed 2 -> u=0.561670 -> c=4 -> out 4 (handgerechnet)");
    cfg.rng_seed = 555;
    CHECK(sample_once(5, 1, 0x01, &cfg, NULL, 0, &out) == 0 && out == 0,
          "Sampler: Seed 555 -> u=0.044097 -> c=2 -> out 0 (handgerechnet)");
    cfg.rng_seed = 0;
    CHECK(sample_once(5, 1, 0x01, &cfg, NULL, 0, &out) == 0 && out == 0,
          "Sampler: Seed 0 wird auf Magic-State gemappt (u=0.052791 -> out 0, kein Degenerat)");
}

static void test_sampler_reproducible(void)
{
    /* Two fresh engines, identical model/config/seed and identical
     * recent-window history -> identical 64-step sequences. */
    char path[64];
    tmp_path_new(path, sizeof(path));
    CHECK(make_v1_model(path, 64, 1, 0x01) == 0, "Sampler: Repro-Modell wird geschrieben");

    hydra_sampler_config_t cfg;
    cfg.temperature = 1.0f; cfg.min_p = 0.2f;
    cfg.repetition_penalty = 1.5f; cfg.penalty_window = 8; cfg.rng_seed = 7;

    int ok = 1;
    uint16_t seq1[64] = {0}, seq2[64] = {0};
    HydraEngine e1, e2;
    if (hydra_engine_load(&e1, path) != 0 || hydra_engine_load(&e2, path) != 0) ok = 0;
    if (ok) {
        uint16_t t1 = 5, t2 = 5;
        for (int s = 0; s < 64 && ok; ++s) {
            size_t n = (size_t)(s > 8 ? 8 : s);
            uint16_t o1, o2;
            if (hydra_sample_token(&e1, t1, &cfg, seq1 + (s - n), n, &o1) != 0 ||
                hydra_sample_token(&e2, t2, &cfg, seq2 + (s - n), n, &o2) != 0) {
                ok = 0; break;
            }
            seq1[s] = o1; seq2[s] = o2;
            if (o1 != o2) ok = 0;
            t1 = o1; t2 = o2;
        }
    }
    hydra_engine_unload(&e1);
    hydra_engine_unload(&e2);
    unlink(path);
    CHECK(ok, "Sampler: identisches Setup + Seed -> bit-identische 64-Step-Sequenz");
}

static void test_sampler_invalid(void)
{
    hydra_sampler_config_t cfg;
    cfg.temperature = 1.0f; cfg.min_p = 0.0f;
    cfg.repetition_penalty = 1.0f; cfg.penalty_window = 1; cfg.rng_seed = 1;
    uint16_t out = 0;

    CHECK(hydra_sample_token(NULL, 0, &cfg, NULL, 0, &out) == -1, "Sampler: NULL-Engine wird abgelehnt");
    HydraEngine dummy;
    memset(&dummy, 0, sizeof(dummy));
    CHECK(hydra_sample_token(&dummy, 0, NULL, NULL, 0, &out) == -1, "Sampler: NULL-Konfig wird abgelehnt");
    CHECK(hydra_sample_token(&dummy, 0, &cfg, NULL, 0, NULL) == -1, "Sampler: NULL-Ausgabe wird abgelehnt");

    /* vocab=1 cannot sample (needs a distribution with >= 2 members). */
    char path[64];
    tmp_path_new(path, sizeof(path));
    CHECK(make_v1_model(path, 1, 1, 0x01) == 0, "Sampler: V=1-Modell wird geschrieben");
    HydraEngine e;
    int ok = hydra_engine_load(&e, path) == 0;
    if (ok) {
        CHECK(hydra_sample_token(&e, 0, &cfg, NULL, 0, &out) == -3,
              "Sampler: Vokabular < 2 wird mit -3 abgelehnt");
        hydra_engine_unload(&e);
    } else {
        CHECK(0, "Sampler: V=1-Modell laedt");
    }
    unlink(path);

    /* temperature out of the numerically safe range. */
    char path2[64];
    tmp_path_new(path2, sizeof(path2));
    CHECK(make_v1_model(path2, 16, 1, 0x01) == 0, "Sampler: V=16-Modell wird geschrieben");
    ok = hydra_engine_load(&e, path2) == 0;
    if (ok) {
        cfg.temperature = 25.0f;
        CHECK(hydra_sample_token(&e, 0, &cfg, NULL, 0, &out) == -4,
              "Sampler: temperature > 20 wird mit -4 abgelehnt (exp-Overflow-Schutz)");
        cfg.temperature = 1.0f;
        cfg.repetition_penalty = 0.5f;
        uint16_t one = 3;
        CHECK(hydra_sample_token(&e, 0, &cfg, &one, 1, &out) == -1,
              "Sampler: penalty < 1.0 wird abgelehnt (kein Discounting)");
        hydra_engine_unload(&e);
    } else {
        CHECK(0, "Sampler: V=16-Modell laedt");
    }
    unlink(path2);
}

/* ---------------------------------------------------------------------
 * M3: AVX2 kernels vs scalar reference (parity tests, same pattern as
 * the NEON comparison). Compiled only into -mavx2 builds; when such a
 * build runs on a CPU without AVX2 the tests print SKIP (the engine
 * itself falls back to scalar via hydra_simd_has_avx2()).
 * ------------------------------------------------------------------- */
#if defined(__AVX2__)
#include "../src/hydra_simd.h"

static void test_avx2_step_kernel(void)
{
    if (!hydra_simd_has_avx2()) {
        printf("SKIP: AVX2 step kernel (build is -mavx2, CPU lacks AVX2)\n");
        return;
    }
    CHECK(hydra_simd_has_avx2(), "AVX2: runtime CPU probe reports AVX2");

    /* Random matrices (platform-fixed LCG, R22) over dims that hit
     * every SIMD/tail combination: below one chunk (8), mid-chunk
     * tails (7, 9, 15, 17, 31, 33, 63) and full chunks (16, 32, 64). */
    static const size_t dims[] = {8, 7, 9, 15, 16, 17, 31, 32, 33, 63, 64};
    int all_match = 1;
    for (size_t di = 0; di < sizeof(dims) / sizeof(dims[0]); ++di) {
        const size_t dim = dims[di];
        int32_t a[HYDRA_EMBED_DIM], b[HYDRA_EMBED_DIM];
        int8_t state[HYDRA_EMBED_DIM];
        int64_t ref[HYDRA_EMBED_DIM] = {0};
        int64_t avx[HYDRA_EMBED_DIM] = {0};
        int32_t token;

        for (size_t i = 0; i < dim; ++i) {
            a[i] = (int32_t)(lcg_byte() % 8193u) - 4096;   /* [-4096, 4096] */
            b[i] = (int32_t)(lcg_byte() % 8193u) - 4096;
            state[i] = (int8_t)((int32_t)(lcg_byte() % 255u) - 127);
        }
        token = (int32_t)(lcg_byte() % 1024u);

        for (size_t i = 0; i < dim; ++i)
            ref[i] = (int64_t)a[i] * token
                   + (int64_t)b[i] * (int32_t)state[i];
        for (size_t i = 0; i + 8 <= dim; i += 8)
            hydra_avx2_accumulate_agg(avx + i, a + i, b + i, state + i, token);
        for (size_t i = dim - (dim % 8); i < dim; ++i)
            avx[i] = (int64_t)a[i] * token
                   + (int64_t)b[i] * (int32_t)state[i];
        if (memcmp(ref, avx, dim * sizeof(int64_t)) != 0) all_match = 0;
    }
    CHECK(all_match,
          "AVX2 step kernel == scalar reference (11 dims, random A/B/state/token, LCG seed)");

    /* Extreme values at the exact layer-cap bound:
     * |4096*1023 + (-4096)*127| = 3,673,024 — the worst case the loader
     * admits, hand-computed; the int32 lanes must hold it exactly. */
    const size_t dim = 8;
    int32_t a[8], b[8];
    int8_t state[8];
    int64_t avx[8] = {0};
    for (size_t i = 0; i < dim; ++i) {
        a[i] = 4096; b[i] = -4096; state[i] = 127;
    }
    hydra_avx2_accumulate_agg(avx, a, b, state, 1023);
    const int64_t expected = (int64_t)4096 * 1023 + (int64_t)(-4096) * 127;
    int exact = 1;
    for (size_t i = 0; i < dim; ++i)
        if (avx[i] != expected) exact = 0;
    CHECK(exact, "AVX2 step kernel: layer-cap extremes (A=4096, t=1023, B=-4096, s=127) exakt");
}

/* Independent scalar reference of the v2 plane unpack (4 weights per
 * byte, weight j at bits 2j..2j+1): the SAME code table as the engine
 * (00=0, 01=+1, 10=-1, 11=0), written from docs/FORMAT.md, not from
 * hydra_build_aggregation_v1_1. */
static void ref_unpack_v2_plane(int32_t *agg, const uint8_t *plane, size_t dim)
{
    static const int8_t map[4] = {0, 1, -1, 0};
    for (size_t i = 0; i < dim / 4; ++i) {
        const uint8_t byte = plane[i];
        for (int j = 0; j < 4; ++j)
            agg[i * 4 + j] += map[(byte >> (2 * j)) & 0x03];
    }
}

static void test_avx2_v2_unpack(void)
{
    if (!hydra_simd_has_avx2()) {
        printf("SKIP: AVX2 v2 unpack (build is -mavx2, CPU lacks AVX2)\n");
        return;
    }

    /* Random planes over 3 layers (the multi-layer ADD path of the
     * kernel) at dim=64 (the engine's supported AVX2 case, 16-byte
     * planes). */
    {
        const size_t dim = 64;
        int32_t ref[HYDRA_EMBED_DIM] = {0};
        int32_t avx[HYDRA_EMBED_DIM] = {0};
        uint8_t plane_a[16], plane_b[16];
        for (uint32_t l = 0; l < 3; ++l) {
            for (size_t i = 0; i < dim / 4; ++i) {
                plane_a[i] = lcg_byte();
                plane_b[i] = lcg_byte();
            }
            ref_unpack_v2_plane(ref, plane_a, dim);
            hydra_avx2_unpack_v2_plane(avx, plane_a, dim);
            ref_unpack_v2_plane(ref, plane_b, dim);
            hydra_avx2_unpack_v2_plane(avx, plane_b, dim);
        }
        CHECK(memcmp(ref, avx, dim * sizeof(int32_t)) == 0,
              "AVX2 v2 unpack == scalar reference (dim 64, 3 random layers)");
    }

    /* All-code sweep: every 2-bit code repeated in all 4 slots of a
     * byte must sum to 4 * map(code) per dimension — the code table
     * itself, pinned per code. */
    for (int code = 0; code < 4; ++code) {
        const size_t dim = 64;
        int32_t ref[HYDRA_EMBED_DIM] = {0};
        int32_t avx[HYDRA_EMBED_DIM] = {0};
        uint8_t plane[16];
        const uint8_t byte = (uint8_t)(code | (code << 2) | (code << 4) | (code << 6));
        for (size_t i = 0; i < dim / 4; ++i) plane[i] = byte;
        ref_unpack_v2_plane(ref, plane, dim);
        hydra_avx2_unpack_v2_plane(avx, plane, dim);
        const int8_t map[4] = {0, 1, -1, 0};
        int ok = (memcmp(ref, avx, dim * sizeof(int32_t)) == 0);
        for (size_t i = 0; ok && i < dim; ++i)
            if (avx[i] != map[code]) ok = 0;
        if (code == 1)
            CHECK(ok, "AVX2 v2 unpack: code 01 -> +1 in all 4 slots/dim, Tafel exakt");
        else if (code == 2)
            CHECK(ok, "AVX2 v2 unpack: code 10 -> -1 in all 4 slots/dim, Tafel exakt");
        else if (code == 0)
            CHECK(ok, "AVX2 v2 unpack: code 00 -> 0 in all 4 slots/dim, Tafel exakt");
        else
            CHECK(ok, "AVX2 v2 unpack: code 11 (reserved) -> 0 in all 4 slots/dim, Tafel exakt");
    }
}
#endif /* __AVX2__ */

/* -------------------------------------------------------------------
 * M4: Transformer primitives (RMSNorm, RoPE, SwiGLU) + streaming KV
 * cache. All expectations are hand-computed (R25: negative controls
 * for every guard; R22: fixed LCG seed for the random inputs).
 * ------------------------------------------------------------------- */

/* LCG float in [-1, 1), 22-bit precision, fixed seed (R22). */
static float lcg_float(void)
{
    static uint32_t st = 0x4D4E4F5Eu; /* "M4NO" */
    st = st * 1664525u + 1013904223u;
    const int32_t s = (int32_t)(st >> 10) - 0x200000; /* [-2^21, 2^21) */
    return (float)s / 2097152.0f;                     /* [-1, 1) */
}

static int float_near(float a, float b, float tol)
{
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d <= tol;
}

static void test_rmsnorm(void)
{
    /* Hand-computed: x=[1,2,3,4], w=1, eps=1e-6.
     * mean(x^2) = (1+4+9+16)/4 = 7.5
     * out[i] = x[i] / sqrt(7.5 + 1e-6) */
    const float x[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float w1[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float out[4] = {0};
    const double inv = 1.0 / sqrt(7.5 + 1e-6);
    CHECK(hydra_rmsnorm(x, w1, 4, 1e-6f, out) == 0 &&
          float_near(out[0], (float)(inv * 1.0), 1e-5f) &&
          float_near(out[1], (float)(inv * 2.0), 1e-5f) &&
          float_near(out[2], (float)(inv * 3.0), 1e-5f) &&
          float_near(out[3], (float)(inv * 4.0), 1e-5f),
          "RMSNorm: hand-computed y = x/sqrt(mean(x^2)+eps) * w");

    /* With weights w=[1,2,1,2]: multiplied per-dim. */
    const float w2[4] = {1.0f, 2.0f, 1.0f, 2.0f};
    float out2[4] = {0};
    CHECK(hydra_rmsnorm(x, w2, 4, 1e-6f, out2) == 0 &&
          float_near(out2[0], (float)(inv * 1.0 * 1.0), 1e-5f) &&
          float_near(out2[1], (float)(inv * 2.0 * 2.0), 1e-5f),
          "RMSNorm: weight vector applied per dimension");

    /* Scale invariance (w=1): rmsnorm(10x) == rmsnorm(x) exactly
     * (the normalization cancels the input scale). */
    const float x10[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    float out10[4] = {0};
    CHECK(hydra_rmsnorm(x10, w1, 4, 1e-6f, out10) == 0 &&
          float_near(out10[0], out[0], 1e-4f) &&
          float_near(out10[3], out[3], 1e-4f),
          "RMSNorm: scale-invariant (rmsnorm(10x) == rmsnorm(x))");

    /* NaN guard (negative control): -1 AND output unmodified. */
    const float xbad[4] = {1.0f, 2.0f, NAN, 4.0f};
    float sentinel[4] = {-999.0f, -999.0f, -999.0f, -999.0f};
    CHECK(hydra_rmsnorm(xbad, w1, 4, 1e-6f, sentinel) == -1 &&
          sentinel[0] == -999.0f && sentinel[2] == -999.0f,
          "RMSNorm: NaN input -> -1, output unmodified");

    /* Argument guards (negative controls). */
    float tmp[1] = {0};
    CHECK(hydra_rmsnorm(x, w1, 0, 1e-6f, tmp) == -1 &&
          hydra_rmsnorm(x, w1, 4, 0.0f, tmp) == -1 &&
          hydra_rmsnorm(NULL, w1, 4, 1e-6f, tmp) == -1,
          "RMSNorm: invalid args (dim=0, eps=0, NULL) -> -1");
}

static void test_rope(void)
{
    /* pos=0 -> identity, bit-exact. */
    float x0[4] = {1.0f, -2.0f, 3.0f, -4.0f};
    CHECK(hydra_rope_inplace(x0, 4, 0, 10000.0) == 0 &&
          x0[0] == 1.0f && x0[1] == -2.0f && x0[3] == -4.0f,
          "RoPE: pos=0 is the identity (bit-exact)");

    /* dim=2, x=[1,0], pos=1, theta=1.0: freq=1, angle=1 rad
     * -> [cos(1), sin(1)] hand-computed. */
    float x1[2] = {1.0f, 0.0f};
    CHECK(hydra_rope_inplace(x1, 2, 1, 1.0) == 0 &&
          float_near(x1[0], (float)cos(1.0), 1e-5f) &&
          float_near(x1[1], (float)sin(1.0), 1e-5f),
          "RoPE: hand-computed rotation (dim=2, pos=1, theta=1)");

    /* Norm preservation: |x'| == |x| (rotation is orthogonal),
     * random input, standard theta=10000. */
    float xr[8];
    double n0 = 0.0;
    for (int i = 0; i < 8; ++i) {
        xr[i] = lcg_float() * 3.0f;
        n0 += (double)xr[i] * xr[i];
    }
    int rc = hydra_rope_inplace(xr, 8, 7, 10000.0);
    double n1 = 0.0;
    for (int i = 0; i < 8; ++i)
        n1 += (double)xr[i] * xr[i];
    CHECK(rc == 0 && float_near((float)sqrt(n1), (float)sqrt(n0), 1e-3f),
          "RoPE: preserves vector norm (orthogonal rotation)");

    /* Guards (negative controls): odd dim, theta=0, NaN unmodified. */
    float xn[3] = {1.0f, 2.0f, 3.0f};
    float xbad[2] = {1.0f, NAN};
    CHECK(hydra_rope_inplace(xn, 3, 1, 10000.0) == -1 &&
          hydra_rope_inplace(xn, 2, 1, 0.0) == -1 &&
          hydra_rope_inplace(xbad, 2, 1, 10000.0) == -1 &&
          xbad[0] == 1.0f && isnan(xbad[1]),
          "RoPE: invalid args (odd dim, theta=0, NaN) -> -1, unmodified");
}

static void test_swiglu(void)
{
    /* g=0 -> silu(0)=0 -> out=0 regardless of u. */
    const float g0[1] = {0.0f};
    const float u5[1] = {5.0f};
    float o0[1] = {77.0f};
    CHECK(hydra_swiglu(g0, u5, 1, o0) == 0 && o0[0] == 0.0f,
          "SwiGLU: g=0 -> 0 (silu(0)=0)");

    /* Hand-computed: g=[1], u=[2] -> 2 * 1 * sigmoid(1)
     * sigmoid(1) = 1/(1+exp(-1)) = 0.7310585786...
     * out = 1.4621171573... */
    const float g1[1] = {1.0f};
    const float u2[1] = {2.0f};
    float o1[1] = {0};
    CHECK(hydra_swiglu(g1, u2, 1, o1) == 0 &&
          float_near(o1[0], (float)(2.0 * 0.7310585786300049), 1e-6f),
          "SwiGLU: hand-computed silu(1)*2");

    /* Tails: g=+100 -> silu ~ 100 (sigmoid ~ 1); g=-100 -> silu ~ 0.
     * No overflow either way (safe sigmoid). */
    const float gp[1] = {100.0f};
    const float gn[1] = {-100.0f};
    float op[1] = {0}, on[1] = {0};
    int tp = hydra_swiglu(gp, u5, 1, op);
    int tn = hydra_swiglu(gn, u5, 1, on);
    float an = on[0];
    if (an < 0.0f) an = -an;
    CHECK(tp == 0 && float_near(op[0], 500.0f, 0.01f) && tn == 0 && an < 1e-30f,
          "SwiGLU: tails (g=+100 -> ~100*u, g=-100 -> ~0, no overflow)");

    /* Bridge to the ternary engine (R14): the float SwiGLU output is
     * the value a feed-forward block feeds back into the engine's
     * int8 state world. Quantize with the SAME clamp the engine uses
     * (v>127 -> 127, v<-127 -> -127, hydra_engine.c) and verify the
     * result is in-range and idempotent — i.e. the bridge cannot
     * produce a value the engine rejects or wraps. */
    int bridge_ok = 1;
    for (int i = 0; i < 256; ++i) {
        float g[1] = {lcg_float() * 8.0f};
        float u[1] = {lcg_float() * 8.0f};
        float y[1] = {0};
        if (hydra_swiglu(g, u, 1, y) != 0) { bridge_ok = 0; break; }
        float v = y[0];
        if (v > 127.0f) v = 127.0f;
        else if (v < -127.0f) v = -127.0f;
        const int8_t q = (int8_t)v;
        /* Re-clamp the quantized value: must be a fixed point. */
        float v2 = (float)q;
        if (v2 > 127.0f) v2 = 127.0f;
        else if (v2 < -127.0f) v2 = -127.0f;
        /* q > 127 is impossible for int8_t; q < -127 catches an
         * out-of-clamp-range quantization (the engine uses [-127,127]). */
        if ((int8_t)v2 != q || q < -127) { bridge_ok = 0; break; }
    }
    CHECK(bridge_ok,
          "SwiGLU->int8 bridge: 256 random outputs clamp to engine-valid int8, idempotent");

    /* Guards (negative controls). */
    const float gbad[1] = {NAN};
    float sentinel[1] = {-999.0f};
    CHECK(hydra_swiglu(gbad, u2, 1, sentinel) == -1 && sentinel[0] == -999.0f &&
          hydra_swiglu(g1, NULL, 1, o1) == -1,
          "SwiGLU: NaN / NULL args -> -1, output unmodified");
}

static void test_kv_cache(void)
{
    /* C=5, S=2, dim=2. Encode token t as k={t,t}, v={-t,t} so every
     * slot is uniquely identifiable. */
    const uint32_t C = 5, S = 2, D = 2;
    static float kbuf[5 * 2], vbuf[5 * 2];
    hydra_kv_cache_t c;

    CHECK(hydra_kv_cache_init(&c, kbuf, vbuf, D, C, S) == 0 &&
          hydra_kv_cache_size(&c) == 0,
          "KV cache: init (C=5, S=2, dim=2), size 0");

    /* Fill to capacity: appends t=0..4. */
    int fill_ok = 1;
    for (uint32_t t = 0; t < C; ++t) {
        float k[2] = {(float)t, (float)t};
        float v[2] = {-(float)t, (float)t};
        if (hydra_kv_cache_append(&c, k, v) != 0) { fill_ok = 0; break; }
    }
    CHECK(fill_ok && hydra_kv_cache_size(&c) == C,
          "KV cache: fills to capacity 5, size 5");

    /* Resident = [t0, t1 (sinks), t2, t3, t4 (window, oldest->newest)]. */
    float kk[2], vv[2];
    int get_ok = 1;
    const uint32_t expect[5] = {0, 1, 2, 3, 4};
    for (uint32_t i = 0; i < C; ++i) {
        if (hydra_kv_cache_get(&c, i, kk, vv) != 0) { get_ok = 0; break; }
        if (kk[0] != (float)expect[i] || vv[0] != -(float)expect[i]) { get_ok = 0; break; }
    }
    CHECK(get_ok, "KV cache: resident order = sinks then window (oldest->newest)");

    /* Append t5: evicts the oldest WINDOW token (t2), never a sink. */
    {
        float k[2] = {5.0f, 5.0f};
        float v[2] = {-5.0f, 5.0f};
        fill_ok = hydra_kv_cache_append(&c, k, v) == 0;
    }
    get_ok = 1;
    const uint32_t expect2[5] = {0, 1, 3, 4, 5};
    for (uint32_t i = 0; i < C; ++i) {
        if (hydra_kv_cache_get(&c, i, kk, vv) != 0) { get_ok = 0; break; }
        if (kk[0] != (float)expect2[i] || vv[0] != -(float)expect2[i]) { get_ok = 0; break; }
    }
    CHECK(fill_ok && get_ok && hydra_kv_cache_size(&c) == C,
          "KV cache: append t5 evicts oldest window token (t2), sink intact");

    /* Append t6..t9: window keeps the LAST 3 (t7,t8,t9); sinks fixed.
     * Trace (window slots 2,3,4): t5->2, t6->3, t7->4, t8->2, t9->3.
     * Oldest resident window append = n - W = 10 - 3 = 7. */
    fill_ok = 1;
    for (uint32_t t = 6; t <= 9; ++t) {
        float k[2] = {(float)t, (float)t};
        float v[2] = {-(float)t, (float)t};
        if (hydra_kv_cache_append(&c, k, v) != 0) { fill_ok = 0; break; }
    }
    get_ok = 1;
    const uint32_t expect3[5] = {0, 1, 7, 8, 9};
    for (uint32_t i = 0; i < C; ++i) {
        if (hydra_kv_cache_get(&c, i, kk, vv) != 0) { get_ok = 0; break; }
        if (kk[0] != (float)expect3[i] || vv[0] != -(float)expect3[i]) { get_ok = 0; break; }
    }
    CHECK(fill_ok && get_ok,
          "KV cache: after t6..t9 resident = [t0,t1,t7,t8,t9] (last 3 window)");

    /* Sinks survive everything (streaming-LLM invariant). */
    get_ok = 1;
    for (uint32_t s = 0; s < S; ++s) {
        if (hydra_kv_cache_sink(&c, s, kk, vv) != 0) { get_ok = 0; break; }
        if (kk[0] != (float)s || vv[0] != -(float)s) { get_ok = 0; break; }
    }
    CHECK(get_ok, "KV cache: sink(0)=t0, sink(1)=t1 after 10 appends (never evicted)");

    /* Negative controls: OOB get, NaN append (no partial write),
     * sink before filled, degenerate init. */
    get_ok = 1;
    if (hydra_kv_cache_get(&c, C, kk, vv) != -1) get_ok = 0;      /* OOB */
    if (hydra_kv_cache_sink(&c, S, kk, vv) != -1) get_ok = 0;     /* i==S */
    {
        const uint32_t before = hydra_kv_cache_size(&c);
        float kb[2] = {1.0f, NAN};
        float vb[2] = {1.0f, 1.0f};
        if (hydra_kv_cache_append(&c, kb, vb) != -1) get_ok = 0;
        if (hydra_kv_cache_size(&c) != before) get_ok = 0;        /* no partial */
    }
    {
        float kb[2] = {1.0f, 1.0f};
        float vb[2] = {1.0f, 1.0f};
        if (hydra_kv_cache_init(&c, kb, vb, D, C, C + 1) != -1) get_ok = 0; /* S>C */
    }
    CHECK(get_ok,
          "KV cache: OOB get, NaN append (no partial), S>capacity all -> -1");
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
    test_prefill_equivalence();
    test_aggregation_matches_layer_loop();
    test_fast_step_matches_full_step();
    test_fast_step_advances_only_dimension_zero();
    test_fast_step_negative_control();
#ifdef __ARM_NEON
    test_neon_matches_scalar();
    test_neon_all_ternary_codes();
#else
    printf("SKIP: NEON-Vergleichstest (nur auf ARM builds mit __ARM_NEON)\n");
#endif
    test_engine_rejects_garbage();
    test_engine_rejects_bounds_violation();
    test_engine_rejects_inconsistent_weights_len();
    test_engine_rejects_layer_overflow();
    test_engine_rejects_offset_in_header();
    test_engine_rejects_symlink();
    test_load_metrics_are_measured();
    test_version_two_format();
    test_axiom();
#if defined(__AVX2__)
    test_avx2_step_kernel();
    test_avx2_v2_unpack();
#endif
    test_sampler_greedy_guard();
    test_sampler_repetition_penalty();
    test_sampler_min_p();
    test_sampler_rng_hand_computed();
    test_sampler_reproducible();
    test_sampler_invalid();
    test_rmsnorm();
    test_rope();
    test_swiglu();
    test_kv_cache();

    printf("\n=== %d Tests, %d failures ===\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}