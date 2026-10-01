/* Hydra Unit-Tests
 *
 * Prueft: Packing-Konsistenz, Decoder, Engine-Step (inkl. seeded
 * Roundtrip-/Varianz-Pruefung), Format-Ablehnung, Axiom-Verifizierung.
 * Build: make test  |  Ausfuehrung: ./hydra-test
 */
#include "hydra_model.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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

static void write_test_model(const char *path, uint32_t dim, uint32_t layers,
                             uint16_t vocab, uint8_t fill)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(1); }

    HydraModelHeader h;
    h.magic = HYDRA_MAGIC;
    h.version = HYDRA_VERSION;
    h.vocab_size = vocab;
    h.dim = dim;
    h.layers = layers;
    h.weights_offset = (uint32_t)sizeof(HydraModelHeader);
    h.weights_len = dim * layers; /* 2 Gewichte pro Byte -> dim*layers Paare */

    fwrite(&h, sizeof(h), 1, f);
    for (uint32_t i = 0; i < h.weights_len; ++i) {
        uint8_t byte = (fill == 0xFFu)
            ? (uint8_t)((i % 4) == 0 ? 0x01u : ((i % 4) == 1 ? 0x02u : 0x00u))
            : fill;
        fputc(byte, f);
    }
    fclose(f);
}

static void test_header_size(void)
{
    /* Packing-Konsistenz C<->Python: Header muss genau 24 Bytes sein */
    CHECK(sizeof(HydraModelHeader) == 24, "Header ist exakt 24 Bytes (struct pack <IHHIIII)");
}

/* Echter End-to-End-Wertetest mit handgerechneten Erwartungswerten:
 * dim=1, layers=1, token=5, state startet 0.
 *   w=0x01 (w1=+1,w2=0): acc = +1*5 + 0*0 =  5 -> out = (5 + 5 + 1) % 100 = 11
 *   w=0x02 (w1=-1,w2=0): acc = -1*5           -> out = (5 + 5 + 1) % 100 = 11
 *   w=0x00 (w1= 0,w2=0): acc =  0             -> out = (0 + 5 + 1) % 100 = 6
 */
static int run_single_weight_case(uint8_t weight_byte, uint16_t token_in,
                                  uint16_t vocab, uint16_t *out)
{
    const char *path = "/tmp/hydra_decode_case.hydra";
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    HydraModelHeader h;
    h.magic = HYDRA_MAGIC;
    h.version = HYDRA_VERSION;
    h.vocab_size = vocab;
    h.dim = 1;
    h.layers = 1;
    h.weights_offset = (uint32_t)sizeof(HydraModelHeader);
    h.weights_len = 1;
    fwrite(&h, sizeof(h), 1, f);
    fputc(weight_byte, f);
    fclose(f);

    HydraEngine e;
    if (hydra_engine_load(&e, path) != 0) return -1;
    int rc = hydra_engine_step(&e, token_in, out);
    hydra_engine_unload(&e);
    return rc;
}

static void test_decoder_known_values(void)
{
    uint16_t out = 0;

    CHECK(run_single_weight_case(0x01, 5, 100, &out) == 0, "Decoder-Case w1=+1 laeuft");
    CHECK(out == 11, "Decoder/Token-Ableitung: w=0x01, token=5 -> 11 (handgerechnet)");

    CHECK(run_single_weight_case(0x02, 5, 100, &out) == 0, "Decoder-Case w1=-1 laeuft");
    CHECK(out == 11, "Decoder/Token-Ableitung: w=0x02, token=5 -> 11 (|acc|)");

    CHECK(run_single_weight_case(0x00, 5, 100, &out) == 0, "Decoder-Case w1=0 laeuft");
    CHECK(out == 6, "Decoder/Token-Ableitung: w=0x00, token=5 -> 6 (nur token+1)");
}

static void test_engine_step(void)
{
    const char *path = "/tmp/hydra_test_model.hydra";
    write_test_model(path, 16, 2, 256, 0xFFu);

    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc == 0, "Load eines gueltigen Modells");
    if (rc != 0) return;

    CHECK(e.header.dim == 16, "Header dim korrekt gelesen");
    CHECK(e.header.vocab_size == 256, "Header vocab korrekt gelesen");

    uint16_t out = 0;
    rc = hydra_engine_step(&e, 7, &out);
    CHECK(rc == 0, "Step liefert Erfolg");
    CHECK(out < 256, "Output-Token innerhalb Vocab");

    hydra_engine_unload(&e);
}

/* Echte Roundtrip-/Varianz-Pruefung (ersetzt den alten Tautologie-Test):
 * - 5 verschiedene Seeds erzeugen zufaellige ternaere Gewichtsmuster
 * - je Seed: 32 Steps, jeder Output muss < vocab sein
 * - je Seed: zweiter Lauf mit frischer Engine muss identische Sequenz
 *   liefern (Reproduzierbarkeit PRO SEED)
 * - ueber Seeds hinweg muessen die Sequenzen variieren (Packing-
 *   Roundtrip und Token-Ableitung reagieren wirklich auf die Gewichte)
 */
#define VARIANCE_SEEDS 5
#define VARIANCE_STEPS 32

static void test_seed_roundtrip_variance(void)
{
    static const uint32_t seeds[VARIANCE_SEEDS] = {
        1u, 42u, 1337u, 0xC0FFEEDBu, 987654321u
    };
    static uint16_t seqs[VARIANCE_SEEDS][VARIANCE_STEPS];
    const char *path = "/tmp/hydra_seed_model.hydra";
    const uint16_t vocab = 256;
    const uint32_t dim = 32, layers = 2;

    for (int s = 0; s < VARIANCE_SEEDS; ++s) {
        /* Zufaellige ternare Gewichte aus Seed erzeugen (2 Codes/Byte,
         * Codes 0/1/2 gleichverteilt, 3 ausgeschlossen -> Format-konform) */
        lcg_seed(seeds[s]);
        FILE *f = fopen(path, "wb");
        if (!f) { perror("fopen"); exit(1); }
        HydraModelHeader h;
        h.magic = HYDRA_MAGIC;
        h.version = HYDRA_VERSION;
        h.vocab_size = vocab;
        h.dim = dim;
        h.layers = layers;
        h.weights_offset = (uint32_t)sizeof(HydraModelHeader);
        h.weights_len = dim * layers;
        fwrite(&h, sizeof(h), 1, f);
        for (uint32_t i = 0; i < h.weights_len; ++i) {
            uint8_t c1 = (uint8_t)(lcg_byte() % 3u);
            uint8_t c2 = (uint8_t)(lcg_byte() % 3u);
            fputc((int)(c1 | (uint8_t)(c2 << 2)), f);
        }
        fclose(f);

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
}

static void test_engine_rejects_garbage(void)
{
    const char *path = "/tmp/hydra_bad_model.hydra";
    FILE *f = fopen(path, "wb");
    if (f) {
        const uint8_t junk[32] = {0xDE,0xAD,0xBE,0xEF};
        fwrite(junk, 1, sizeof(junk), f);
        fclose(f);
    }
    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc != 0, "Load mit falschem Magic wird abgelehnt");
    hydra_engine_unload(&e);
}

static void test_engine_rejects_bounds_violation(void)
{
    /* Header behauptet mehr Gewichte als die Datei hergibt */
    const char *path = "/tmp/hydra_oob_model.hydra";
    FILE *f = fopen(path, "wb");
    if (f) {
        HydraModelHeader h;
        h.magic = HYDRA_MAGIC;
        h.version = HYDRA_VERSION;
        h.vocab_size = 64;
        h.dim = 64;
        h.layers = 4;
        h.weights_offset = sizeof(HydraModelHeader);
        h.weights_len = 1024; /* Datei ist kleiner! */
        fwrite(&h, sizeof(h), 1, f);
        fclose(f);
    }
    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc != 0, "Load mit Out-of-Bounds-Weights wird abgelehnt");
    hydra_engine_unload(&e);
}

static void test_engine_rejects_inconsistent_weights_len(void)
{
    /* PoC-Regressions-Test: layers*dim > weights_len muss abgelehnt werden,
     * sonst liest step() hinter das mmap (OOB-Read). */
    const char *path = "/tmp/hydra_poc_model.hydra";
    FILE *f = fopen(path, "wb");
    if (f) {
        HydraModelHeader h;
        h.magic = HYDRA_MAGIC;
        h.version = HYDRA_VERSION;
        h.vocab_size = 64;
        h.dim = 64;
        h.layers = 4;               /* step wuerde 4*64 = 256 Bytes lesen */
        h.weights_offset = sizeof(HydraModelHeader);
        h.weights_len = 8;          /* Datei enthaelt nur 8 Gewicht-Bytes */
        fwrite(&h, sizeof(h), 1, f);
        const uint8_t w[8] = {1,1,1,1,1,1,1,1};
        fwrite(w, 1, sizeof(w), f);
        fclose(f);
    }
    HydraEngine e;
    int rc = hydra_engine_load(&e, path);
    CHECK(rc != 0, "Load mit layers*dim > weights_len wird abgelehnt (OOB-PoC)");
    hydra_engine_unload(&e);
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

    CHECK(hydra_verify_axiom(1.0f, 1.0f, NULL) == -1, "NULL-Score-Pointer abgelehnt");
}

int main(void)
{
    printf("=== Hydra Engine Unit-Tests ===\n\n");

    test_header_size();
    test_decoder_known_values();
    test_engine_step();
    test_seed_roundtrip_variance();
    test_engine_rejects_garbage();
    test_engine_rejects_bounds_violation();
    test_engine_rejects_inconsistent_weights_len();
    test_axiom();

    printf("\n=== %d Tests, %d Fehler ===\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
