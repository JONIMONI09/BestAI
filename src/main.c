#include "hydra_model.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Verwendung: %s <modell.hydra> [start_token]\n", argv[0]);
        printf("Tipp: Erzeuge vorher ein Modell mit: python tools/make_dummy_model.py\n");
        return 1;
    }

    const char *model_path = argv[1];
    uint16_t current_token = (argc > 2) ? (uint16_t)atoi(argv[2]) : 42;

    HydraEngine engine;
    if (hydra_engine_load(&engine, model_path) != 0) {
        return 1;
    }

    printf("[Hydra] Starte O(1) Inferenzschleife. Eingangs-Token: %u\n", current_token);
    printf("[Tokens]: ");

    for (int step = 0; step < 16; ++step) {
        uint16_t next_token = 0;
        hydra_engine_step(&engine, current_token, &next_token);
        printf("%u ", next_token);
        current_token = next_token;
    }
    printf("\n");

    /* Axiom-Test */
    float safe_val = 0.0f;
    int ok = hydra_verify_axiom(1.0f, 99.5f, &safe_val);
    printf("[Axiom-Check Mensch=1.0]: Erlaubt=%d, Score=%.2f\n", ok, safe_val);

    ok = hydra_verify_axiom(0.0f, 99.5f, &safe_val);
    printf("[Axiom-Check Mensch=0.0]: Erlaubt=%d, Score=%.2f (Blockiert!)\n", ok, safe_val);

    hydra_engine_unload(&engine);
    printf("[Hydra] Beendet. Speicher sauber freigegeben.\n");
    return 0;
}
