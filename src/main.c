#include "hydra_model.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Hydra-Stone CLI
 *
 * Verwendung:
 *   hydra-run <modell.hydra> [start_token] [steps] [--json] [--prompt t1,t2,...]
 *
 * Standard: menschenlesbare Ausgabe auf stdout, Engine-Logs auf stderr.
 * --json:   maschinenlesbares JSON (tokens, state, timing) fuer die Web-UI.
 * --prompt: komma-getrennte Token-IDs, die VOR der Generierung durch die
 *           Engine geschickt werden (Prefill). Der letzte Prompt-Token ist
 *           der Seed; steps zaehlen wie bisher nur die erzeugten Tokens.
 *           Ohne --prompt gilt start_token als Seed (unveraendertes
 *           Verhalten).
 * --help:   Verwendung und Exit-Code 0.
 */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void usage(const char *argv0, FILE *out)
{
    fprintf(out, "Verwendung: %s <modell.hydra> [start_token] [steps] [--json] "
                 "[--prompt t1,t2,...]\n", argv0);
    fprintf(out, "\n");
    fprintf(out, "  <modell.hydra>   Pfad zu einer .hydra-Modell-Datei (Pflicht)\n");
    fprintf(out, "  start_token      Start-Token-ID, 0..vocab-1 (Standard: 42)\n");
    fprintf(out, "  steps            Anzahl erzeugter Inferenzschritte, 1..256 (Standard: 16)\n");
    fprintf(out, "  --json           Maschinenlesbare Ausgabe auf stdout\n");
    fprintf(out, "  --prompt LIST    Komma-getrennte Token-IDs als Prefill vor der Generierung\n");
    fprintf(out, "  --bench [N]      Benchmark: N Schritte (Default 10000), JSON auf stdout\n");
    fprintf(out, "  --fast           Token-only Fast Path, nur Dimension 0\n");
    fprintf(out, "  --help           Diese Hilfe anzeigen\n");
    fprintf(out, "\n");
    fprintf(out, "Beispiel: %s models/starter.hydra 123 32 --json\n", argv0);
    fprintf(out, "Beispiel: %s models/starter.hydra 0 8 --prompt 7,9,11 --json\n", argv0);
    fprintf(out, "Tipp: Modell erzeugen mit: python3 tools/make_model.py <ziel>\n");
}

/* strtol mit strengem Zahlencheck: kein Unsinn, kein Overflow, kein
 * stillschweigend gekapptes Grenzwert-Literal. */
static int parse_long_arg(const char *arg, long *out)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(arg, &end, 10);
    if (end == arg || *end != '\0' || errno == ERANGE || v < 0) return -1;
    *out = v;
    return 0;
}

/* --prompt parsen: streng. Leere Eintraege, Nicht-Ziffern, Ueberlaenge
 * und Werte oberhalb von 65535 werden abgewiesen statt stillschweigend
 * gekappt - ein gekappter Seed waere ein falsches Modellresultat. */
#define MAX_PROMPT 256

static int parse_prompt(const char *arg, uint16_t *out, size_t cap, size_t *n_out)
{
    const char *p = arg;
    size_t n = 0;
    if (!arg || !*arg) return -1;
    while (*p) {
        char *end = NULL;
        errno = 0;
        long v = strtol(p, &end, 10);
        if (end == p || errno == ERANGE || v < 0 || v > 65535) return -1;
        if (n >= cap) {
            fprintf(stderr, "[Hydra] Prompt zu lang (max %d Tokens)\n", MAX_PROMPT);
            return -1;
        }
        out[n++] = (uint16_t)v;
        if (*end == '\0') break;
        if (*end != ',') {
            fprintf(stderr, "[Hydra] Prompt-Fehler an '%s' (erwartet: Ziffern, Kommas)\n", end);
            return -1;
        }
        p = end + 1;
        if (*p == '\0') { /* trailing comma */
            fprintf(stderr, "[Hydra] Prompt endet auf einem Komma\n");
            return -1;
        }
    }
    if (n == 0) return -1;
    *n_out = n;
    return 0;
}

static void print_json(const HydraEngine *e, const uint16_t *tokens,
                       int n, uint16_t start, double elapsed_ms,
                       const uint16_t *prompt, size_t prompt_n)
{
    printf("{\"tokens\":[");
    for (int i = 0; i < n; ++i) {
        printf("%s%u", i ? "," : "", (unsigned)tokens[i]);
    }
    printf("],\"state\":[");
    for (uint32_t i = 0; i < e->header.dim; ++i) {
        printf("%s%d", i ? "," : "", (int)e->state_vector[i]);
    }
    printf("],\"dim\":%u,\"vocab\":%u,\"layers\":%u,\"start_token\":%u,"
           "\"steps\":%d,\"elapsed_ms\":%.3f",
           e->header.dim, e->header.vocab_size, e->header.layers,
           (unsigned)start, n, elapsed_ms);
    /* Ladezeit und Gewichtsgroesse mitgeben: die Konsole zeigt sie als
     * Modell-Chip an, und eine Behauptung ueber Ladezeiten braucht einen
     * Wert, der im selben Aufruf gemessen wurde. */
    printf(",\"load_ms\":%.3f,\"weights_bytes\":%zu",
           e->load_ms, e->weights_bytes);
    /* Das Prompt wird mit ausgegeben: die Web-UI muss nachweisen koennen,
     * dass wirklich die ganze Sequenz in die Engine gegangen ist und
     * nicht nur ihr erstes Token. */
    printf(",\"prompt\":[");
    for (size_t i = 0; i < prompt_n; ++i) {
        printf("%s%u", i ? "," : "", (unsigned)prompt[i]);
    }
    printf("]}");
}

/* ------------------------------------------------------------------ */
/* Benchmark                                                            */
/* ------------------------------------------------------------------ */

/* Phase-0-Messung: ein Aufruf, drei Pfade, eine JSON-Zeile.
 *
 *   native_loop : reiner C-Step im Prozess, ohne JNI, ohne Callback.
 *                  Das ist die untere Schranke - alles andere ist
 *                  mindestens so teuer.
 *   fast_token  : derselbe Aufruf ueber hydra_engine_step_fast (nur
 *                  Dimension 0), um den Hebel messbar zu machen.
 *
 * Der JNI-Anteil (Callback pro Token vs. gebuendelt) kann hier nicht
 * gemessen werden - dafuer gibt es HydraBridge.benchmark() in der
 * Android-App. Diese Zahl fehlt in CLI-Benchmarks bewusst; sie wird
 * nicht geschaetzt.
 *
 * Warmup: ein kompletter Durchlauf ueber das Modell, damit die
 * mmap-Seiten im Page-Cache liegen. Ohne Warmup misst man in erster
 * Linie den Page-Cache, nicht den Kernel. */
static double bench_run(const char *model_path, uint16_t start_token, long steps,
                        int fast_mode, uint16_t *first_out, HydraEngine *stats)
{
    HydraEngine e;
    if (hydra_engine_load(&e, model_path) != 0) return -1.0;
    if (stats) *stats = e;

    /* Warmup: ein voller Durchlauf, damit die mmap-Seiten im Page-Cache
     * liegen. Ohne das misst man den Page-Cache, nicht den Kernel. */
    uint16_t tok = start_token;
    for (long i = 0; i < e.header.layers; ++i) {
        uint16_t n = 0;
        if (fast_mode) { if (hydra_engine_step_fast(&e, tok, &n) != 0) { hydra_engine_unload(&e); return -1.0; } }
        else if (hydra_engine_step(&e, tok, &n) != 0) { hydra_engine_unload(&e); return -1.0; }
        tok = n;
    }

    /* Messung. clock_gettime(CLOCK_MONOTONIC) statt clock(): clock() misst
     * CPU-Zeit und schliesst Treiberwartezeit aus - genau die Zeit, die
     * eine GPU-Vergleichsmessung zeigen soll. */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long s = 0; s < steps; ++s) {
        uint16_t n = 0;
        if (fast_mode) { if (hydra_engine_step_fast(&e, tok, &n) != 0) { hydra_engine_unload(&e); return -1.0; } }
        else if (hydra_engine_step(&e, tok, &n) != 0) { hydra_engine_unload(&e); return -1.0; }
        tok = n;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ns = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 +
                 (double)(t1.tv_nsec - t0.tv_nsec)) / (double)steps;
    if (first_out) *first_out = tok;
    hydra_engine_unload(&e);
    return ns;
}

static int run_benchmark(const char *model_path, const HydraModelHeader *h,
                         uint16_t start_token, long steps)
{

    if (steps < 1) {
        fprintf(stderr, "[Hydra] --bench braucht mindestens 1 Schritt\n");
        return 1;
    }

    /* Jeder Pfad laedt sein eigenes Modell: der State-Vektor ist
     * Zustand, und ein zweiter Lauf auf einem gesättigten Vektor ist ein
     * anderes Experiment als ein frischer Start. */
    uint16_t sink = 0;
    HydraEngine full_stats;
    double ns_full = bench_run(model_path, start_token, steps, 0, &sink, &full_stats);
    if (ns_full < 0) { fprintf(stderr, "[Hydra] Bench: Modell konnte nicht geladen werden\n"); return 1; }
    uint16_t sink_fast = 0;
    double ns_fast = bench_run(model_path, start_token, steps, 1, &sink_fast, NULL);
    if (ns_fast < 0) { fprintf(stderr, "[Hydra] Bench: Fast Path fehlgeschlagen\n"); return 1; }

    /* Beide Ströme muessen identisch sein - sonst vergleicht der Benchmark
     * zwei verschiedene Algorithmen und die Zahlen sind Muell.
     *
     * Die Ladezahlen stehen mit drin, weil "Modelle laden schnell" eine
     * Behauptung ist und eine Behauptung braucht eine Messung:
     *   load_ms                 gemessene Ladezeit inkl. Aggregation
     *   weights_bytes            Bytes, die die Aggregation liest
     *   rss_delta_kb             RSS Aenderung ueber das Laden hinweg
     * Ein rss_delta_kb von 0 heisst: nach dem Laden ist keine einzige
     * Gewichtsseite mehr im Prozess - MADV_SEQUENTIAL gibt sie hinter dem
     * Lesezeiger frei. Genau das ist die Messung hinter der O(1)-Aussage;
     * ein MADV_DONTNEED zusaetzlich wurde gebaut, gemessen und verworfen
     * (Kommentar in src/hydra_engine.c). */
    long rss_delta = -1;
    if (full_stats.rss_kb_before_load >= 0 && full_stats.resident_kb_after_load >= 0) {
        rss_delta = full_stats.resident_kb_after_load - full_stats.rss_kb_before_load;
    }
    printf("{\"mode\":\"bench\",\"engine\":\"cpu\",\"model\":\"%s\","
           "\"dim\":%u,\"layers\":%u,\"vocab\":%u,\"steps\":%ld,"
           "\"warmup_layers\":%u,"
           "\"ns_per_token_full\":%.2f,\"ns_per_token_fast\":%.2f,"
           "\"speedup\":%.3f,\"tokens_equal\":%s,"
           "\"load_ms\":%.3f,\"weights_bytes\":%zu,"
           "\"rss_before_kb\":%ld,"
           "\"rss_after_kb\":%ld,\"rss_delta_kb\":%ld,"
           "\"note\":\"JNI callback costs are NOT included here; measure them with HydraBridge.benchmark() on the device\"}",
           model_path, h->dim, h->layers, h->vocab_size, steps,
           h->layers, ns_full, ns_fast, ns_fast > 0 ? ns_full / ns_fast : 0.0,
           sink == sink_fast ? "true" : "false",
           full_stats.load_ms, full_stats.weights_bytes,
           full_stats.rss_kb_before_load,
           full_stats.resident_kb_after_load, rss_delta);
    printf("\n");
    return sink == sink_fast ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *model_path = NULL;
    long start_token = -1;   /* Sentinel: noch nicht gesetzt */
    long steps = -1;
    int json_mode = 0;
    int fast_mode = 0;
    int bench_mode = 0;
    long bench_steps = 10000;
    uint16_t prompt[MAX_PROMPT];
    size_t prompt_n = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) {
            json_mode = 1;
        } else if (strcmp(argv[i], "--fast") == 0) {
            fast_mode = 1;
        } else if (strcmp(argv[i], "--bench") == 0) {
            bench_mode = 1;
            /* Optionales N direkt nach --bench; ein Following-Token wie
             * "--json" wird nicht als Zahl akzeptiert. */
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                if (parse_long_arg(argv[i + 1], &bench_steps) != 0) {
                    fprintf(stderr, "[Hydra] --bench erwartet eine positive Ganzzahl\n");
                    return 1;
                }
                ++i;
            }
        } else if (strcmp(argv[i], "--prompt") == 0) {
            /* Fehlendes Argument explizit abweisen: argv[i+1] == NULL ist
             * ein Benutzerfehler, kein leeres Prompt. */
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "[Hydra] --prompt braucht eine kommagetrennte Token-Liste\n");
                usage(argv[0], stderr);
                return 1;
            }
            if (parse_prompt(argv[i + 1], prompt, MAX_PROMPT, &prompt_n) != 0) {
                fprintf(stderr, "[Hydra] Ungueltiges --prompt: '%s'\n", argv[i + 1]);
                return 1;
            }
            ++i;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0], stdout);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "[Hydra] Unbekannte Option: %s\n", argv[i]);
            usage(argv[0], stderr);
            return 1;
        } else if (!model_path) {
            model_path = argv[i];
        } else {
            long v;
            if (parse_long_arg(argv[i], &v) != 0) {
                fprintf(stderr, "[Hydra] Ungueltiges Argument: %s "
                                "(erwartet: nichtnegative Ganzzahl <= %ld)\n",
                        argv[i], LONG_MAX);
                return 1;
            }
            if (start_token == -1) start_token = v;
            else if (steps == -1)  steps = v;
            else {
                /* Zusaetzliche Positionsargumente werden nicht stillschweigend
                 * verworfen (BUG-12) — ein Tippfehler faellt sonst nie auf. */
                fprintf(stderr, "[Hydra] Zu viele Argumente: %s "
                                "(max. <modell> [start_token] [steps])\n", argv[i]);
                usage(argv[0], stderr);
                return 1;
            }
        }
    }

    if (!model_path) {
        fprintf(stderr, "[Hydra] Fehlende Modelldatei.\n");
        usage(argv[0], stderr);
        return 1;
    }

    /* Defaults fuer optionale Positional-Argumente; ausserhalb liegender
     * Werte werden explizit geklemmt und nicht mehr stillschweigend
     * akzeptiert. */
    if (start_token == -1) start_token = 42;
    if (steps == -1) steps = 16;
    if (steps < 1)   steps = 1;
    if (steps > 256) steps = 256;

    /* --prompt gewinnt ueber das positionelle start_token: der Seed ist
     * dann das LETZTE Prompt-Token. Das ist explizit, nicht still. */
    if (prompt_n > 0) start_token = prompt[prompt_n - 1];

    HydraEngine engine;
    if (hydra_engine_load(&engine, model_path) != 0) {
        return 1;
    }

    if (start_token >= (long)engine.header.vocab_size) {
        start_token %= engine.header.vocab_size;
    }

    uint16_t *tokens = malloc(sizeof(uint16_t) * (size_t)steps);
    if (!tokens) {
        hydra_engine_unload(&engine);
        return 1;
    }

    if (bench_mode) {
        const int rc = run_benchmark(model_path, &engine.header,
                                     (uint16_t)start_token, bench_steps);
        free(tokens);
        hydra_engine_unload(&engine);
        return rc;
    }

    uint16_t current = (uint16_t)start_token;

    /* Prefill zuerst: das Prompt laeuft durch die Engine, danach wird
     * generiert. Die Prompt-Tokens selbst werden NICHT ausgegeben -
     * steps bleibt die Anzahl der erzeugten Tokens. */
    if (prompt_n > 0) {
        if (hydra_engine_prefill(&engine, prompt, prompt_n) != 0) {
            free(tokens);
            hydra_engine_unload(&engine);
            return 1;
        }
    }

    double t0 = now_ms();
    for (long s = 0; s < steps; ++s) {
        uint16_t next = 0;
        const int step_rc = fast_mode
            ? hydra_engine_step_fast(&engine, current, &next)
            : hydra_engine_step(&engine, current, &next);
        if (step_rc != 0) {
            free(tokens);
            hydra_engine_unload(&engine);
            return 1;
        }
        tokens[s] = next;
        current = next;
    }
    double elapsed = now_ms() - t0;

    if (json_mode) {
        print_json(&engine, tokens, (int)steps, (uint16_t)start_token, elapsed,
                   prompt, prompt_n);
        printf("\n");
    } else {
        printf("[Hydra] Starte O(1) Inferenzschleife. Eingangs-Token: %ld, Steps: %ld\n",
               start_token, steps);
        if (prompt_n > 0) {
            printf("[Prompt]: ");
            for (size_t p = 0; p < prompt_n; ++p) printf("%u ", (unsigned)prompt[p]);
            printf("\n");
        }
        printf("[Tokens]: ");
        for (long s = 0; s < steps; ++s) {
            printf("%u ", (unsigned)tokens[s]);
        }
        printf("\n[Timing]: %.3f ms gesamt, %.3f ms/Token\n",
               elapsed, steps ? elapsed / (double)steps : 0.0);

        /* Axiom-Demo */
        float safe_val = 0.0f;
        int ok = hydra_verify_axiom(1.0f, 99.5f, &safe_val);
        printf("[Axiom-Check Mensch=1.0]: Erlaubt=%d, Score=%.2f\n", ok, (double)safe_val);
        ok = hydra_verify_axiom(0.0f, 99.5f, &safe_val);
        printf("[Axiom-Check Mensch=0.0]: Erlaubt=%d, Score=%.2f (Blockiert!)\n",
               ok, (double)safe_val);
    }

    free(tokens);
    hydra_engine_unload(&engine);
    return 0;
}