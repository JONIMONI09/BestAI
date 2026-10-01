#include "hydra_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Hydra-Stone CLI
 *
 * Verwendung:
 *   hydra-run <modell.hydra> [start_token] [steps] [--json]
 *
 * Standard: menschenlesbare Ausgabe auf stdout, Engine-Logs auf stderr.
 * --json:   maschinenlesbares JSON (tokens, state, timing) fuer die Web-UI.
 */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void print_json(const HydraEngine *e, const uint16_t *tokens,
                       int n, uint16_t start, double elapsed_ms)
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
           "\"steps\":%d,\"elapsed_ms\":%.3f}",
           e->header.dim, e->header.vocab_size, e->header.layers,
           (unsigned)start, n, elapsed_ms);
}

int main(int argc, char **argv)
{
    const char *model_path = NULL;
    long start_token = -1;   /* Sentinel: noch nicht gesetzt */
    long steps = -1;
    int json_mode = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) {
            json_mode = 1;
        } else if (!model_path) {
            model_path = argv[i];
        } else {
            char *end = NULL;
            long v = strtol(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 0) {
                fprintf(stderr, "[Hydra] Ungueltiges Argument: %s\n", argv[i]);
                return 1;
            }
            if (start_token == -1) start_token = v;
            else if (steps == -1)  steps = v;
        }
    }

    if (!model_path) {
        fprintf(stderr, "Verwendung: %s <modell.hydra> [start_token] [steps] [--json]\n", argv[0]);
        fprintf(stderr, "Tipp: Erzeuge vorher ein Modell mit: python3 tools/make_dummy_model.py\n");
        return 1;
    }

    /* Defaults fuer optionale Positional-Argumente */
    if (start_token == -1) start_token = 42;
    if (steps == -1) steps = 16;
    if (steps < 1)   steps = 1;
    if (steps > 256) steps = 256;

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

    uint16_t current = (uint16_t)start_token;
    double t0 = now_ms();
    for (long s = 0; s < steps; ++s) {
        uint16_t next = 0;
        if (hydra_engine_step(&engine, current, &next) != 0) {
            free(tokens);
            hydra_engine_unload(&engine);
            return 1;
        }
        tokens[s] = next;
        current = next;
    }
    double elapsed = now_ms() - t0;

    if (json_mode) {
        print_json(&engine, tokens, (int)steps, (uint16_t)start_token, elapsed);
        printf("\n");
    } else {
        printf("[Hydra] Starte O(1) Inferenzschleife. Eingangs-Token: %ld, Steps: %ld\n",
               start_token, steps);
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
