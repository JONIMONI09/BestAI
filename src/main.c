/* Feature-test macros: under a strict -std=c99 compile glibc hides
 * struct timespec / clock_gettime (POSIX 1b) behind these. Defining
 * them in the source file keeps the file self-contained instead of
 * relying on every build adding -D_POSIX_C_SOURCE=200809L
 * -D_DEFAULT_SOURCE (same finding as in hydra_engine.c,
 * guarded against command-line redefinition, 2026-10-26). */
#if !defined(__APPLE__) && !defined(__MACH__)
/* glibc (Linux) + Bionic (Android): under a strict compile these enable
 * the POSIX symbols the code uses. On Apple this block must stay OFF:
 * Darwin headers interpret _POSIX_C_SOURCE as a restriction and hide
 * the BSD extensions O_NOFOLLOW and MADV_SEQUENTIAL (neither is in any
 * POSIX version), which broke release.yml's macOS build 2026-10-26. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif /* !__APPLE__ && !__MACH__ */

#include "hydra_model.h"
#include "hydra_v2_container.h"
#include "hydra_moe.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
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
    fprintf(out, "  --text-prompt TEXT  Text als BPE-Prompt (.hydra2 mit eingebettetem Vokabular, M4);\n"
                 "                   wechselseitig ausschliesslich mit --prompt\n");
    fprintf(out, "  --bench [N]      Benchmark: N Schritte (Default 10000), JSON auf stdout\n");
    fprintf(out, "  --fast           Token-only Fast Path, nur Dimension 0\n");
    fprintf(out, "  --inspect        Modell-Datei analysieren (JSON, .hydra UND .hydra2)\n");
    fprintf(out, "  --temp F         Sampler-Temperatur (Standard 0 = greedy); > 0 aktiviert Sampling\n");
    fprintf(out, "  --min-p F        Min-P-Filter in [0,1] (Standard 0); P(c) < min-p*p_max wird verworfen\n");
    fprintf(out, "  --rep-penalty F  Wiederholungs-Strafe, >= 1.0 (Standard 1.0 = aus)\n");
    fprintf(out, "  --rep-window N   Fenster der Wiederholungs-Strafe in Tokens, 0..4096 (Standard 64)\n");
    fprintf(out, "  --seed N         RNG-Seed fuer den Sampler (Standard 0)\n");
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

/* Sampler flag values are floats; the parser rejects garbage, signs of
 * infinity/NaN, and out-of-range values the same way parse_long_arg
 * rejects bad integers. Range rules are enforced by the caller because
 * they are sampler semantics (temperature <= 0 means "greedy", not an
 * error). */
static int parse_float_arg(const char *arg, double *out)
{
    char *end = NULL;
    errno = 0;
    double v = strtod(arg, &end);
    if (end == arg || *end != '\0' || errno == ERANGE || !isfinite(v))
        return -1;
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
/* Which path bench_run() measures. */
typedef enum { HYDRA_BENCH_FULL = 0, HYDRA_BENCH_FAST = 1 } BenchMode;

/* What to run. These three used to be three adjacent scalar parameters
 * (uint16_t token, long steps, int mode). Every one of them implicitly
 * converts into the others, so `bench_run(path, steps, token, 1)` compiles,
 * runs, and reports a plausible number for the wrong experiment
 * (bugprone-easily-swappable-parameters). Bundling them makes the mistake
 * impossible to express: the fields are named, not positional. */
typedef struct {
    uint16_t start_token;
    long     steps;
    BenchMode mode;
} BenchRequest;

static double bench_run(const char *model_path, const BenchRequest *req,
                        uint16_t *first_out, HydraEngine *stats)
{
    HydraEngine e;
    /* Zeroed so that `*stats = e` copies a fully-defined struct: the loader
     * fills the fields it needs and the caller prints others, and copying
     * indeterminate padding into a struct that is later printed is a real
     * way to hand the analyser (and valgrind) undefined bytes. */
    memset(&e, 0, sizeof e);
    /* Publish a defined "not measured" state BEFORE any early return. The
     * load-failure path below returns without touching *stats, so a caller
     * that looked at it first would read uninitialised stack. -1 is the
     * engine's own "unknown" marker and the caller already tests for it. */
    if (stats) { memset(stats, 0, sizeof *stats); stats->rss_kb_before_load = -1; stats->resident_kb_after_load = -1; }
    if (hydra_engine_load(&e, model_path) != 0) return -1.0;
    if (stats) *stats = e;

    /* Warmup: ein voller Durchlauf, damit die mmap-Seiten im Page-Cache
     * liegen. Ohne das misst man den Page-Cache, nicht den Kernel. */
    uint16_t tok = req->start_token;
    for (long i = 0; i < e.header.layers; ++i) {
        uint16_t n = 0;
        if (e.header.version >= 3) {
            const int rc = (e.header.version == 4)
                ? hydra_transformer_forward_step(&e, tok, &n, NULL)
                : hydra_engine_step_tf(&e, tok, &n, NULL);
            if (rc != 0) { hydra_engine_unload(&e); return -1.0; }
        }
        else if (req->mode) { if (hydra_engine_step_fast(&e, tok, &n) != 0) { hydra_engine_unload(&e); return -1.0; } }
        else if (hydra_engine_step(&e, tok, &n) != 0) { hydra_engine_unload(&e); return -1.0; }
        tok = n;
    }

    /* Messung. clock_gettime(CLOCK_MONOTONIC) statt clock(): clock() misst
     * CPU-Zeit und schliesst Treiberwartezeit aus - genau die Zeit, die
     * eine GPU-Vergleichsmessung zeigen soll. */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long s = 0; s < req->steps; ++s) {
        uint16_t n = 0;
        if (e.header.version >= 3) {
            const int rc = (e.header.version == 4)
                ? hydra_transformer_forward_step(&e, tok, &n, NULL)
                : hydra_engine_step_tf(&e, tok, &n, NULL);
            if (rc != 0) { hydra_engine_unload(&e); return -1.0; }
        }
        else if (req->mode) { if (hydra_engine_step_fast(&e, tok, &n) != 0) { hydra_engine_unload(&e); return -1.0; } }
        else if (hydra_engine_step(&e, tok, &n) != 0) { hydra_engine_unload(&e); return -1.0; }
        tok = n;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ns = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 +
                 (double)(t1.tv_nsec - t0.tv_nsec)) / (double)req->steps;
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
    /* Zeroed via memset (not `= {0}`) so the file stays warning-free
     * under a C++ parse (g++ -Wmissing-field-initializers). */
    HydraEngine full_stats;  /* bench_run publishes the real values, or -1 */
    memset(&full_stats, 0, sizeof full_stats);
    const BenchRequest full_req = { start_token, steps, HYDRA_BENCH_FULL };
    double ns_full = bench_run(model_path, &full_req, &sink, &full_stats);
    if (ns_full < 0) { fprintf(stderr, "[Hydra] Bench: Modell konnte nicht geladen werden\n"); return 1; }
    uint16_t sink_fast = 0;
    double ns_fast = -1.0;
    if (h->version >= 3) {
        /* v3/v4 have no aggregated legacy path to compare against: the
         * per-layer non-linear pass is the ONLY path, so the fast
         * benchmark is reported as not applicable (0.0) instead of
         * measuring a different algorithm. */
        sink_fast = sink;
        ns_fast = 0.0;
    } else {
        const BenchRequest fast_req = { start_token, steps, HYDRA_BENCH_FAST };
        ns_fast = bench_run(model_path, &fast_req, &sink_fast, NULL);
        if (ns_fast < 0) { fprintf(stderr, "[Hydra] Bench: Fast Path fehlgeschlagen\n"); return 1; }
    }

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
    const char *bench_note = (h->version >= 3)
        ? "v3/v4 per-layer pass; ns_per_token_fast=0 means the legacy fast path does not exist for v3/v4 models; JNI callback costs are NOT included here"
        : "JNI callback costs are NOT included here; measure them with HydraBridge.benchmark() on the device";
    printf("{\"mode\":\"bench\",\"engine\":\"cpu\",\"model\":\"%s\","
           "\"version\":%u,\"dim\":%u,\"layers\":%u,\"vocab\":%u,\"steps\":%ld,"
           "\"warmup_layers\":%u,"
           "\"ns_per_token_full\":%.2f,\"ns_per_token_fast\":%.2f,"
           "\"speedup\":%.3f,\"tokens_equal\":%s,"
           "\"load_ms\":%.3f,\"weights_bytes\":%zu,"
           "\"rss_before_kb\":%ld,"
           "\"rss_after_kb\":%ld,\"rss_delta_kb\":%ld,"
           "\"note\":\"%s\"}",
           model_path, h->version, h->dim, h->layers, h->vocab_size, steps,
           h->layers, ns_full, ns_fast, ns_fast > 0 ? ns_full / ns_fast : 0.0,
           sink == sink_fast ? "true" : "false",
           full_stats.load_ms, full_stats.weights_bytes,
           full_stats.rss_kb_before_load,
           full_stats.resident_kb_after_load, rss_delta, bench_note);
    printf("\n");
    return sink == sink_fast ? 0 : 1;
}

/* --------------------------------------------------------------------
 * --inspect: the model analysis report, for BOTH formats.
 *
 * This is the ONE place the .hydra2 container reader is consumed by
 * hydra-run (FORMAT-V2.md "Implementation status" listed inspection as
 * the missing consumer). The report is the reader's own verdict - the
 * same validation the MoE engine will run - not a second, weaker
 * implementation. Exit codes: 0 = the format's reader accepts the
 * file (load_ok for .hydra), 2 = refused, 1 = usage/IO error.
 * -------------------------------------------------------------------- */
static void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 6 < cap; ++i) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch < 0x20u) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", ch);
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = '\0';
}

/* A metadata value is raw bytes; render it by its evident type:
 * 4/8 bytes -> unsigned number, printable ASCII -> string, else hex. */
static void meta_value_json(const uint8_t *v, uint32_t n, char *out, size_t cap)
{
    if (n == 4) {
        uint32_t x = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
                     ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
        snprintf(out, cap, "%u", (unsigned)x);
        return;
    }
    if (n == 8) {
        uint64_t x = 0;
        for (int i = 7; i >= 0; --i) x = (x << 8) | v[i];
        snprintf(out, cap, "%llu", (unsigned long long)x);
        return;
    }
    int printable = 1;
    for (uint32_t i = 0; i < n; ++i)
        if (v[i] < 0x21u || v[i] > 0x7Eu) { printable = 0; break; }
    /* Round-13 (Qodo #18): every branch returns a COMPLETE JSON
     * value - numbers bare, strings and hex blobs already quoted.
     * The pre-fix split contract (strings unquoted, blobs quoted)
     * was re-guessed at the call site with a first-character test
     * that double-quoted any binary value starting with a hex digit
     * other than '0' (e.g. "ab..." -> ""ab...""). */
    if (printable) {
        char esc[256];
        if (n >= sizeof(esc)) n = (uint32_t)(sizeof(esc) - 1);
        memcpy(esc, v, n);
        esc[n] = '\0';
        char tmp[sizeof(esc)];
        json_escape(esc, tmp, sizeof tmp);
        snprintf(out, cap, "\"%s\"", tmp);
        return;
    }
    char tmp[2 * 256 + 8];
    size_t o = 0;
    tmp[o++] = '"';
    for (uint32_t i = 0; i < n && o + 3 < sizeof tmp; ++i)
        o += (size_t)snprintf(tmp + o, sizeof tmp - o, "%02x", v[i]);
    tmp[o++] = '"';
    tmp[o] = '\0';
    snprintf(out, cap, "%s", tmp);
}

static int inspect_hydra_v1(const char *path, FILE *f, size_t file_bytes)
{
    HydraEngine engine;
    int rc = hydra_engine_load(&engine, path);
    if (rc != 0) {
        /* The loader already named the exact rule on stderr. */
        fprintf(f,
            "{\"format\":\"hydra\",\"file_bytes\":%zu,"
            "\"load\":{\"ok\":false,\"rc\":%d}}\n", file_bytes, rc);
        return 2;
    }
    const HydraModelHeader *h = &engine.header;
    fprintf(f,
        "{\"format\":\"hydra\",\"file_bytes\":%zu,"
        "\"magic_hex\":\"0x%08x\",\"version\":%u,\"vocab\":%u,"
        "\"dim\":%u,\"layers\":%u,\"weights_offset\":%u,"
        "\"weights_len\":%u,\"load\":{\"ok\":true}}\n",
        file_bytes, (unsigned)h->magic, (unsigned)h->version,
        (unsigned)h->vocab_size, (unsigned)h->dim, (unsigned)h->layers,
        (unsigned)h->weights_offset, (unsigned)h->weights_len);
    hydra_engine_unload(&engine);
    return 0;
}

static int inspect_hydra_v2(const char *path, FILE *f, size_t file_bytes)
{
    hydra2_container_t c;
    int rc = hydra2_open(&c, path);
    if (rc != HYDRA2_OK) {
        /* The reader already named field/value/bound on stderr. */
        fprintf(f, "{\"format\":\"hydra2\",\"file_bytes\":%zu,"
                   "\"reader\":\"refused\",\"error_code\":%d}\n",
                file_bytes, rc);
        return 2;
    }
    char name[64], val[512];
    fprintf(f,
        "{\"format\":\"hydra2\",\"file_bytes\":%zu,"
        "\"version\":{\"major\":%u,\"minor\":%u},\"header_bytes\":%u,"
        "\"reader\":\"ok\",\"trunk_backend\":\"%s\","
        "\"regions\":{\"metadata\":{\"offset\":%llu,\"bytes\":%llu},"
        "\"tensor_dir\":{\"offset\":%llu,\"bytes\":%llu},"
        "\"expert_dir\":{\"offset\":%llu,\"bytes\":%llu},"
        "\"trunk\":{\"offset\":%llu,\"bytes\":%llu},"
        "\"experts\":{\"offset\":%llu,\"bytes\":%llu}},"
        "\"routing\":{\"architecture\":\"%s\",\"layers\":%u,"
        "\"moe_layers\":%u,\"top_k\":%u,\"experts_per_layer\":%u,"
        "\"router_dtype\":\"%s\",\"expert_bytes_typ\":%llu,"
        "\"router_runs_ahead\":%u},"
        "\"metadata\":[",
        file_bytes, (unsigned)c.version_major, (unsigned)c.version_minor,
        (unsigned)c.header_bytes,
        c.trunk_backend,
        (unsigned long long)c.metadata_offset,
        (unsigned long long)c.metadata_bytes,
        (unsigned long long)c.tensor_dir_offset,
        (unsigned long long)c.tensor_dir_bytes,
        (unsigned long long)c.expert_dir_offset,
        (unsigned long long)c.expert_dir_bytes,
        (unsigned long long)c.trunk_offset,
        (unsigned long long)c.trunk_bytes,
        (unsigned long long)c.experts_offset,
        (unsigned long long)c.experts_bytes,
        c.architecture, (unsigned)c.layers, (unsigned)c.moe_layers,
        (unsigned)c.top_k, (unsigned)c.experts_per_layer,
        hydra2_type_name(c.router_dtype),
        (unsigned long long)c.expert_bytes_typ,
        (unsigned)c.router_runs_ahead);
    for (uint32_t i = 0; i < c.meta_count; ++i) {
        const hydra2_meta_kv_t *kv = &c.meta[i];
        /* The blob is NOT NUL-terminated (key bytes are immediately
         * followed by the value bytes): bound the copy by key_len. */
        char keybuf[64];
        uint32_t klen = kv->key_len;
        if (klen >= sizeof(keybuf)) klen = (uint32_t)(sizeof(keybuf) - 1);
        memcpy(keybuf, c.meta_blob + kv->key_off, klen);
        keybuf[klen] = '\0';
        json_escape(keybuf, name, sizeof name);
        meta_value_json(c.meta_blob + kv->val_off, kv->val_len, val, sizeof val);
        /* val is a complete JSON value (meta_value_json contract) */
        fprintf(f, "%s{\"key\":\"%s\",\"value\":%s}", i ? "," : "", name, val);
    }
    fprintf(f, "],\"tensors\":[");
    for (uint64_t i = 0; i < c.tensor_dir_count; ++i) {
        const hydra2_tensor_entry_t *t = &c.tensors[i];
        json_escape(t->name, name, sizeof name);
        fprintf(f, "%s{\"name\":\"%s\",\"role\":\"%s\",\"type\":\"%s\","
                     "\"n_dims\":%u,\"n_elements\":%llu,\"n_bytes\":%llu,"
                     "\"trunk_offset\":%llu}",
                i ? "," : "", name, hydra2_role_name(t->role),
                hydra2_type_name(t->type_id), (unsigned)t->n_dims,
                (unsigned long long)t->n_elements,
                (unsigned long long)t->n_bytes,
                (unsigned long long)t->offset);
    }
    fprintf(f, "],\"experts\":{\"count\":%llu,\"sample\":[",
            (unsigned long long)c.expert_dir_count);
    uint64_t shown = 0;
    for (uint64_t i = 0; i < c.expert_dir_count && shown < 8; ++i, ++shown) {
        const hydra2_expert_entry_t *e = &c.experts[i];
        fprintf(f, "%s{\"layer\":%u,\"expert_id\":%u,\"offset\":%llu,"
                     "\"length\":%llu,\"type\":\"%s\",\"crc32\":\"0x%08x\"}",
                shown ? "," : "", (unsigned)e->layer, (unsigned)e->expert_id,
                (unsigned long long)e->offset, (unsigned long long)e->length,
                hydra2_type_name((uint16_t)e->type_id), (unsigned)e->crc32);
    }
    fprintf(f, "]}}\n");
    hydra2_close(&c);
    return 0;
}

static int inspect_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Round 13 (Qodo #25): the error must NOT echo the path -
         * inspect output is what tools display to users, and a failed
         * read must not leak the local filesystem layout. */
        fprintf(stderr,
                "[Hydra] --inspect: Datei nicht lesbar (fehlende Datei, "
                "Zugriffsrechte oder kein reguläres Dateisystem-Objekt)\n");
        return 1;
    }
    unsigned char head[8];
    size_t got = fread(head, 1, 8, f);
    long szl = 0;
    if (fseek(f, 0, SEEK_END) == 0) szl = ftell(f);
    fclose(f);
    if (szl < 0) szl = 0;
    const size_t file_bytes = (size_t)szl;

    if (got < 8) {
        printf("{\"format\":\"unknown\",\"file_bytes\":%zu,"
               "\"error\":\"shorter than the 8-byte magic\"}\n", file_bytes);
        return 2;
    }
    if (memcmp(head, HYDRA2_MAGIC_STR, HYDRA2_MAGIC_LEN) == 0)
        return inspect_hydra_v2(path, stdout, file_bytes);
    if (got >= 4 && *(uint32_t *)&head[0] == HYDRA_MAGIC)
        return inspect_hydra_v1(path, stdout, file_bytes);
    char hex[17];
    for (int i = 0; i < 8; ++i)
        snprintf(hex + 2 * i, 3, "%02x", head[i]);
    printf("{\"format\":\"unknown\",\"file_bytes\":%zu,"
           "\"magic_hex\":\"%s\",\"error\":\"not a .hydra or .hydra2 "
           "file\"}\n", file_bytes, hex);
    return 2;
}

/* ------------------------------------------------------------------ */
/* .hydra2 execution: the F32 sparse-MoE engine (round 12)            */
/* ------------------------------------------------------------------ */

/* Minimal JSON string escaper for the --text-prompt echo (M4): quotes,
 * backslashes and control characters. */
static void json_print_string(const char *t)
{
    for (const uint8_t *p = (const uint8_t *)t; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar((int)*p);
        } else if (*p == '\n') {
            fputs("\\n", stdout);
        } else if (*p == '\t') {
            fputs("\\t", stdout);
        } else if (*p == '\r') {
            fputs("\\r", stdout);
        } else if (*p < 0x20u) {
            printf("\\u%04x", (unsigned)*p);
        } else {
            putchar((int)*p);
        }
    }
}

/* Magic sniff: is this a .hydra2 file? (the run path dispatches on it;
 * the v1-v4 loader keeps its own validation). */
static int file_is_hydra2(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char head[8];
    size_t got = fread(head, 1, 8, f);
    fclose(f);
    return got == 8 && memcmp(head, HYDRA2_MAGIC_STR, HYDRA2_MAGIC_LEN) == 0;
}

/* Run a .hydra2 model through the MoE engine with the CLI's existing
 * contract: prompt prefill (not printed), `steps` generated tokens,
 * greedy by default, the v1-v4 sampler flags mapped onto the REAL
 * per-vocabulary logit vector (which this engine produces). --fast has
 * no meaning for the F32 path and is announced, not applied.
 * text_prompt (M4): non-NULL when --text-prompt was given; the text is
 * BPE-encoded with the model's embedded vocabulary and replaces the
 * token prompt. */
static int run_moe_file(const char *model_path, long start_token, long steps,
                        int json_mode, int fast_mode,
                        uint16_t *prompt, size_t prompt_n,
                        const char *text_prompt,
                        double temperature, double min_p,
                        double rep_penalty, uint32_t penalty_window,
                        uint64_t rng_seed, int sampler_set)
{
    hydra_moe_t m;
    int rc = hydra_moe_open(model_path, &m);
    if (rc != 0) {
        fprintf(stderr, "[MoE] %s konnte nicht zum Inferieren geoeffnet werden (rc=%d)\n",
                model_path, rc);
        return 1;
    }
    hydra_moe_config_t cfg;
    hydra_moe_config(&m, &cfg);

    if (fast_mode) {
        fprintf(stderr,
                "[Hydra] Hinweis: --fast ist bei .hydra2-Modellen nicht anwendbar "
                "(F32-Pfad ohne Aggregat-Legacy-Variante)\n");
    }

    /* prompt tokens are uint16 from the CLI; the MoE vocab can be
     * smaller - validate loudly, do not wrap silently */
    for (size_t p = 0; p < prompt_n; ++p) {
        if (prompt[p] >= cfg.vocab_size) {
            fprintf(stderr,
                    "[MoE] Prompt-Token %zu (%u) liegt ausserhalb des Vokabulars "
                    "(vocab_size=%u)\n", p, (unsigned)prompt[p], cfg.vocab_size);
            hydra_moe_close(&m);
            return 1;
        }
    }
    if (start_token >= (long)cfg.vocab_size)
        start_token %= (long)cfg.vocab_size;

    if (text_prompt) {
        if (!hydra_moe_has_vocab(&m)) {
            fprintf(stderr,
                    "[MoE] --text-prompt braucht ein eingebettetes Vokabular "
                    "(Tensor 'vocab', M4); diese Datei hat keins\n");
            hydra_moe_close(&m);
            return 1;
        }
        if (cfg.vocab_size > 65535u) {
            fprintf(stderr,
                    "[MoE] --text-prompt: vocab_size %u uebersteigt den "
                    "CLI-Promptbereich (uint16)\n", (unsigned)cfg.vocab_size);
            hydra_moe_close(&m);
            return 1;
        }
        uint32_t *tt = (uint32_t *)malloc((size_t)MAX_PROMPT * sizeof(uint32_t));
        if (!tt) { hydra_moe_close(&m); return 1; }
        int tn = hydra_moe_encode_text(&m, text_prompt, tt, MAX_PROMPT);
        if (tn < 0) {
            fprintf(stderr,
                    "[MoE] --text-prompt konnte nicht kodiert werden (rc=%d)\n",
                    tn);
            free(tt);
            hydra_moe_close(&m);
            return 1;
        }
        for (int p = 0; p < tn; ++p)
            prompt[p] = (uint16_t)tt[p];
        prompt_n = (size_t)tn;
        free(tt);
    }

    /* KV storage: one cache per layer, the model's own sink/ring
     * geometry (the engine appends; the CLI owns the memory). */
    const uint32_t n_layers = cfg.layers;
    const uint32_t kvd = cfg.n_kv_heads * cfg.head_dim;
    const uint32_t cap = cfg.kv_sink + cfg.kv_ring;
    float *kv_k = NULL, *kv_v = NULL;
    hydra_kv_cache_t *kvs = NULL;
    if (n_layers > 0) {
        const size_t per = (size_t)cap * (size_t)kvd * sizeof(float);
        kv_k = (float *)malloc(per * n_layers);
        kv_v = (float *)malloc(per * n_layers);
        kvs = (hydra_kv_cache_t *)calloc(n_layers, sizeof(hydra_kv_cache_t));
        if (!kv_k || !kv_v || !kvs) {
            fprintf(stderr, "[MoE] KV-Initialisierung fehlgeschlagen\n");
            free(kv_k); free(kv_v); free(kvs);
            hydra_moe_close(&m);
            return 1;
        }
        for (uint32_t l = 0; l < n_layers; ++l) {
            if (hydra_kv_cache_init(&kvs[l],
                    kv_k + (size_t)l * cap * kvd,
                    kv_v + (size_t)l * cap * kvd,
                    kvd, cap, cfg.kv_sink) != 0) {
                fprintf(stderr, "[MoE] KV-Initialisierung fehlgeschlagen\n");
                free(kv_k); free(kv_v); free(kvs);
                hydra_moe_close(&m);
                return 1;
            }
        }
    }

    /* recent-token ring for the repetition penalty: prompt tokens
     * count, then every generated token. Round-13 fixes (Qodo #16/#22):
     * the write position is a MONOTONIC counter modulo the window -
     * the old code indexed by recent_n clamped at the window, so after
     * the first wraparound every write hit slot 0 (the "last" window
     * of tokens was wrong), and a window of 0 hit `x % 0` (SIGFPE).
     * A full ring is passed to the sampler rotated; the sampler only
     * counts occurrences per token, so the rotation is immaterial. */
    uint32_t *recent = NULL;
    uint32_t recent_n = 0, recent_pos = 0;
    if (penalty_window > 0) {
        recent = (uint32_t *)malloc((size_t)penalty_window * sizeof(uint32_t));
        if (!recent) {
            free(kv_k); free(kv_v); free(kvs);
            hydra_moe_close(&m);
            return 1;
        }
        for (size_t p = 0; p < prompt_n; ++p) {
            recent[recent_pos] = prompt[p];
            recent_pos = (recent_pos + 1) % penalty_window;
            if (recent_n < penalty_window) recent_n++;
        }
    }

    uint32_t *tokens = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)steps);
    if (!tokens) {
        free(recent); free(kv_k); free(kv_v); free(kvs);
        hydra_moe_close(&m);
        return 1;
    }
    float *logits = (float *)malloc((size_t)cfg.vocab_size * sizeof(float));
    if (!logits) {
        free(tokens); free(recent); free(kv_k); free(kv_v); free(kvs);
        hydra_moe_close(&m);
        return 1;
    }

    const int sampling_enabled = temperature > 0.0 && min_p < 1.0;
    uint64_t rng = rng_seed ? rng_seed : 0x9E3779B97F4A7C15ull;

    /* Round-13 fix (Qodo #30): the prompt now CONDITIONS the model -
     * every prompt token except the last is run through the engine to
     * build the KV history, and generation continues from the LAST
     * prompt token. Pre-fix the prompt only seeded the repetition ring
     * and the display; the generated stream ignored it entirely (the
     * forward loop started at pos = prompt_n with current = the CLI
     * start token, so the KV cache was empty and "conditioned on the
     * prompt" was a lie). With an empty prompt this reduces to exactly
     * the pre-fix behaviour (current = start_token, pos = 0). */
    int fail = 0;
    uint32_t pos = 0;
    for (size_t p = 0; p + 1u < prompt_n; ++p) {
        if (hydra_moe_forward(&m, prompt[p], pos, logits, kvs) != 0) {
            fail = 1;
            break;
        }
        ++pos;
    }
    uint32_t current = (prompt_n > 0) ? (uint32_t)prompt[prompt_n - 1]
                                      : (uint32_t)start_token;

    double t0 = now_ms();
    for (long st = 0; st < steps; ++st) {
        if (hydra_moe_forward(&m, current, pos, logits, kvs) != 0) { fail = 1; break; }
        /* the greedy guard lives in hydra_moe_sample itself:
         * temperature <= 0 or min_p >= 1 -> argmax, no RNG consumed */
        if (hydra_moe_sample(logits, cfg.vocab_size, &current,
                             temperature, min_p, rep_penalty,
                             (uint32_t)recent_n,
                             recent_n ? recent : NULL, &rng) != 0) { fail = 1; break; }
        tokens[st] = current;
        if (penalty_window > 0) {
            recent[recent_pos] = current;
            recent_pos = (recent_pos + 1) % penalty_window;
            if (recent_n < penalty_window) recent_n++;
        }
        ++pos;
    }
    double elapsed = now_ms() - t0;
    if (fail) {
        fprintf(stderr, "[MoE] Inferenz fehlgeschlagen\n");
        free(logits); free(tokens); free(recent); free(kv_k); free(kv_v); free(kvs);
        hydra_moe_close(&m);
        return 1;
    }

    /* Round-13 fix (Qodo #7): the generated stream is reported as
     * DECODED text, not only token ids - the decoder exists
     * (hydra_moe_decode_token) but the CLI never called it, so users
     * saw "10 5 10 5 ..." and had to guess the vocabulary. Only when
     * the model carries an embedded vocabulary; a token without a
     * string (should not happen) is skipped, not fatal. */
    char *out_text = (char *)malloc(256);
    size_t out_cap = out_text ? 256u : 0u, out_len = 0;
    if (hydra_moe_has_vocab(&m)) {
        char tbuf[1024];
        for (long st = 0; st < steps; ++st) {
            const int tl = hydra_moe_decode_token(&m, tokens[st], tbuf,
                                                  (uint32_t)sizeof tbuf);
            if (tl < 0) continue;
            while (out_len + (size_t)tl + 1u > out_cap) {
                const size_t nc = out_cap ? out_cap * 2u : 256u;
                char *nb = (char *)realloc(out_text, nc);
                if (!nb) break;
                out_text = nb;
                out_cap = nc;
            }
            if (out_len + (size_t)tl + 1u > out_cap) break;
            memcpy(out_text + out_len, tbuf, (size_t)tl);
            out_len += (size_t)tl;
            out_text[out_len++] = 0;
        }
        if (out_len > 0) out_text[out_len - 1] = 0;
    }

    if (json_mode) {
        printf("{\"format\":\"hydra2-moe\",\"tokens\":[");
        for (long st = 0; st < steps; ++st)
            printf("%s%u", st ? "," : "", (unsigned)tokens[st]);
        printf("],\"dim\":%u,\"vocab\":%u,\"layers\":%u,\"top_k\":%u,"
               "\"n_heads\":%u,\"n_kv_heads\":%u,\"head_dim\":%u,"
               "\"inter\":%u,\n",
               cfg.dim, cfg.vocab_size, cfg.layers, cfg.top_k,
               cfg.n_heads, cfg.n_kv_heads, cfg.head_dim, cfg.inter);
        printf("\"start_token\":%u,\"steps\":%ld,\"elapsed_ms\":%.3f,"
               "\"prompt\":[",
               (unsigned)start_token, steps, elapsed);
        for (size_t p = 0; p < prompt_n; ++p)
            printf("%s%u", p ? "," : "", (unsigned)prompt[p]);
        printf("]");
        if (text_prompt) {
            printf(",\"text\":\"");
            json_print_string(text_prompt);
            printf("\"");
        }
        if (hydra_moe_has_vocab(&m)) {
            printf(",\"generated_text\":\"");
            json_print_string(out_len ? out_text : "");
            printf("\"");
        }
        printf(",\"sampler\":{\"temperature\":%.4g,\"min_p\":%.4g,"
               "\"repetition_penalty\":%.4g,\"penalty_window\":%u,"
               "\"seed\":%llu}}\n",
               temperature, min_p, rep_penalty, (unsigned)penalty_window,
               (unsigned long long)rng_seed);
    } else {
        printf("[Hydra] .hydra2 MoE-Inferenz. Eingangs-Token: %ld, Steps: %ld\n",
               start_token, steps);
        if (text_prompt)
            printf("[Text]: %s\n", text_prompt);
        if (prompt_n > 0) {
            printf("[Prompt]: ");
            for (size_t p = 0; p < prompt_n; ++p) printf("%u ", (unsigned)prompt[p]);
            printf("\n");
        }
        if (sampler_set || sampling_enabled) {
            printf("[Sampler]: temp=%.4g min-p=%.4g rep-penalty=%.4g rep-window=%u seed=%llu%s\n",
                   temperature, min_p, rep_penalty, (unsigned)penalty_window,
                   (unsigned long long)rng_seed,
                   sampling_enabled ? "" : " (greedy guard aktiv)");
        }
        printf("[Tokens]: ");
        for (long st = 0; st < steps; ++st) printf("%u ", (unsigned)tokens[st]);
        printf("\n");
        if (hydra_moe_has_vocab(&m))
            printf("[Text-out]: %s\n", out_len ? out_text : "");
        printf("[Timing]: %.3f ms gesamt, %.3f ms/Token\n",
               elapsed, steps ? elapsed / (double)steps : 0.0);
    }

    free(out_text);
    free(logits); free(tokens); free(recent); free(kv_k); free(kv_v); free(kvs);
    hydra_moe_close(&m);
    return 0;
}

int main(int argc, char **argv)
{
    const char *model_path = NULL;
    long start_token = -1;   /* Sentinel: noch nicht gesetzt */
    long steps = -1;
    int json_mode = 0;
    int fast_mode = 0;
    int inspect_mode = 0;
    int bench_mode = 0;
    long bench_steps = 10000;
    uint16_t prompt[MAX_PROMPT];
    size_t prompt_n = 0;
    const char *text_prompt = NULL;

    /* Sampler: defaults are the greedy guard (temperature 0, min_p 0),
     * so a plain invocation is bit-identical to the pre-sampling CLI.
     * Sampling activates only when temperature > 0 AND min_p < 1. */
    hydra_sampler_config_t sampler;
    sampler.temperature = 0.0f;
    sampler.min_p = 0.0f;
    sampler.repetition_penalty = 1.0f;
    sampler.penalty_window = 64;
    sampler.rng_seed = 0;
    int sampler_set = 0; /* any --temp/--min-p/--rep-penalty/--rep-window/--seed seen */

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) {
            json_mode = 1;
        } else if (strcmp(argv[i], "--fast") == 0) {
            fast_mode = 1;
        } else if (strcmp(argv[i], "--inspect") == 0) {
            inspect_mode = 1;
        } else if (strcmp(argv[i], "--temp") == 0) {
            double v;
            if (i + 1 >= argc || parse_float_arg(argv[i + 1], &v) != 0) {
                fprintf(stderr, "[Hydra] --temp erwartet eine Fließkomma-Zahl (0 = greedy)\n");
                return 1;
            }
            sampler.temperature = (float)v;
            sampler_set = 1;
            ++i;
        } else if (strcmp(argv[i], "--min-p") == 0) {
            double v;
            if (i + 1 >= argc || parse_float_arg(argv[i + 1], &v) != 0) {
                fprintf(stderr, "[Hydra] --min-p erwartet eine Fließkomma-Zahl in [0,1]\n");
                return 1;
            }
            if (v < 0.0 || v > 1.0) {
                fprintf(stderr, "[Hydra] --min-p muss in [0,1] liegen\n");
                return 1;
            }
            sampler.min_p = (float)v;
            sampler_set = 1;
            ++i;
        } else if (strcmp(argv[i], "--rep-penalty") == 0) {
            double v;
            if (i + 1 >= argc || parse_float_arg(argv[i + 1], &v) != 0) {
                fprintf(stderr, "[Hydra] --rep-penalty erwartet eine Zahl >= 1.0 (1.0 = aus)\n");
                return 1;
            }
            if (v < 1.0) {
                fprintf(stderr, "[Hydra] --rep-penalty muss >= 1.0 sein\n");
                return 1;
            }
            sampler.repetition_penalty = (float)v;
            sampler_set = 1;
            ++i;
        } else if (strcmp(argv[i], "--rep-window") == 0) {
            long w;
            if (i + 1 >= argc || parse_long_arg(argv[i + 1], &w) != 0 || w > 4096) {
                fprintf(stderr, "[Hydra] --rep-window erwartet eine Ganzzahl 0..4096\n");
                return 1;
            }
            sampler.penalty_window = (uint16_t)w;
            sampler_set = 1;
            ++i;
        } else if (strcmp(argv[i], "--seed") == 0) {
            long s;
            if (i + 1 >= argc || parse_long_arg(argv[i + 1], &s) != 0 || s > 4294967295L) {
                fprintf(stderr, "[Hydra] --seed erwartet eine Ganzzahl 0..4294967295\n");
                return 1;
            }
            sampler.rng_seed = (uint32_t)s;
            sampler_set = 1;
            ++i;
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
        } else if (strcmp(argv[i], "--text-prompt") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "[Hydra] --text-prompt braucht einen Text\n");
                usage(argv[0], stderr);
                return 1;
            }
            if (prompt_n > 0) {
                fprintf(stderr,
                        "[Hydra] --text-prompt und --prompt sind wechselseitig "
                        "ausschliesslich\n");
                return 1;
            }
            text_prompt = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--prompt") == 0) {
            /* Fehlendes Argument explizit abweisen: argv[i+1] == NULL ist
             * ein Benutzerfehler, kein leeres Prompt. */
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "[Hydra] --prompt braucht eine kommagetrennte Token-Liste\n");
                usage(argv[0], stderr);
                return 1;
            }
            if (text_prompt) {
                fprintf(stderr,
                        "[Hydra] --text-prompt und --prompt sind wechselseitig "
                        "ausschliesslich\n");
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

    if (inspect_mode) {
        return inspect_file(model_path);
    }

    /* M4: --text-prompt exists only for .hydra2 files with an embedded
     * vocabulary - the v1 .hydra format stores no token strings. */
    if (text_prompt && !file_is_hydra2(model_path)) {
        fprintf(stderr,
                "[Hydra] --text-prompt braucht ein .hydra2-Modell mit "
                "eingebettetem Vokabular; die v1 .hydra-Formate speichern "
                "keine Token-Strings\n");
        return 1;
    }
    if (text_prompt && bench_mode) {
        fprintf(stderr,
                "[Hydra] --text-prompt und --bench sind nicht kombinierbar\n");
        return 1;
    }

    /* Defaults fuer optionale Positional-Argumente; ausserhalb liegender
     * Werte werden explizit geklemmt und nicht mehr stillschweigend
     * akzeptiert. Round 13 (Qodo #23): this now precedes the magic
     * dispatch so it applies to BOTH engine paths - the .hydra2
     * dispatch used to run with start_token = -1 / steps = -1 when
     * the positional arguments were omitted. */
    if (start_token == -1) start_token = 42;
    if (steps == -1) steps = 16;
    if (steps < 1)   steps = 1;
    if (steps > 256) steps = 256;

    /* --prompt gewinnt ueber das positionelle start_token: der Seed ist
     * dann das LETZTE Prompt-Token. Das ist explizit, nicht still. */
    if (prompt_n > 0) start_token = prompt[prompt_n - 1];

    /* Magic dispatch (round 12): a .hydra2 file runs on the F32
     * sparse-MoE engine (src/hydra_moe.c), NOT on the v1-v4 loader -
     * the two formats share nothing at runtime. The v1-v4 path below
     * is the pre-round-12 code, untouched. */
    if (file_is_hydra2(model_path)) {
        return run_moe_file(model_path, start_token, steps, json_mode,
                            fast_mode, prompt, prompt_n, text_prompt,
                            sampler.temperature, sampler.min_p,
                            sampler.repetition_penalty,
                            sampler.penalty_window,
                            (uint64_t)sampler.rng_seed, sampler_set);
    }

    HydraEngine engine;
    if (hydra_engine_load(&engine, model_path) != 0) {
        return 1;
    }

    if (start_token >= (long)engine.header.vocab_size) {
        start_token %= engine.header.vocab_size;
    }

    /* The explicit cast keeps this file parseable as C++ (g++
     * -fsyntax-only -fanalyzer), where implicit void* -> T* conversion
     * is ill-formed; in C the cast is a no-op. */
    uint16_t *tokens = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)steps);
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

    /* Sampling is active only when the greedy guard is NOT hit:
     * temperature > 0 AND min_p < 1. Otherwise hydra_sample_token()
     * would just delegate to the native step, so the loop keeps the
     * original call shape for the default (greedy) invocation. */
    const int sampling_enabled =
        sampler.temperature > 0.0f && sampler.min_p < 1.0f;
    if (sampling_enabled && fast_mode && engine.header.version < 3) {
        /* --fast only exists for the greedy path: sampling advances
         * the full state through hydra_engine_step() by contract, so
         * the fast path is ignored. Say so instead of being quiet. */
        fprintf(stderr,
                "[Hydra] Hinweis: --fast wird ignoriert, weil Sampling aktiv ist "
                "(temp=%g, min-p=%g)\n",
                (double)sampler.temperature, (double)sampler.min_p);
    }

    /* Version 3/4: the CLI owns the KV storage the engine appends to
     * (the engine itself stays zero-heap; see hydra_engine_step_tf /
     * hydra_transformer_forward_step).
     * 512-slot ring + 1 permanent sink covers the CLI's max of 256
     * steps; sinks keep the first state resident for the whole run
     * (v3: attention-sink residual; v4: the attended sink, see
     * docs/ARCHITECTURE.md). */
    int v3 = (engine.header.version == 3);
    int v4 = (engine.header.version == 4);
    /* Number of KV caches: 1 for v3 (a single shared ring), `layers`
     * for v4 (one cache per layer, because every layer has its own
     * Wq/Wk/Wv and attends to its own memory), 0 for v1/v2. */
    const uint32_t n_kv =
        (uint32_t)(v4 ? engine.header.layers : (v3 ? 1u : 0u));
    float *kv_k = NULL, *kv_v = NULL;
    hydra_kv_cache_t *kv_arr = NULL;
    if (n_kv > 0) {
        const uint32_t cap = 512, nsink = 1, dimf = engine.header.dim;
        const size_t per = (size_t)(cap + nsink) * (size_t)dimf * sizeof(float);
        kv_k = (float *)malloc(per * (size_t)n_kv);
        kv_v = (float *)malloc(per * (size_t)n_kv);
        kv_arr = (hydra_kv_cache_t *)calloc(n_kv, sizeof(hydra_kv_cache_t));
        if (!kv_k || !kv_v || !kv_arr) {
            fprintf(stderr, "[Hydra] v3/v4: KV-Initialisierung fehlgeschlagen\n");
            free(kv_k); free(kv_v); free(kv_arr); free(tokens);
            hydra_engine_unload(&engine);
            return 1;
        }
        for (uint32_t l = 0; l < n_kv; ++l) {
            if (hydra_kv_cache_init(&kv_arr[l],
                    kv_k + (size_t)l * (size_t)(cap + nsink) * (size_t)dimf,
                    kv_v + (size_t)l * (size_t)(cap + nsink) * (size_t)dimf,
                    dimf, cap, nsink) != 0) {
                fprintf(stderr, "[Hydra] v3/v4: KV-Initialisierung fehlgeschlagen\n");
                free(kv_k); free(kv_v); free(kv_arr); free(tokens);
                hydra_engine_unload(&engine);
                return 1;
            }
        }
        if (fast_mode) {
            /* v3 has no aggregated legacy path at all: --fast is not
             * applicable, the per-layer pass is the model. Say so,
             * then run the real thing instead of failing silently. */
            fprintf(stderr,
                "[Hydra] Hinweis: --fast ist bei v3/v4-Modellen nicht anwendbar "
                "(kein Aggregat-Legacy-Pfad); es laeuft der volle per-layer pass\n");
            fast_mode = 0;
        }
    }
    /* The two views the loop below dispatches on: v3 uses the single
     * shared ring, v4 the per-layer array. Both NULL for v1/v2. */
    hydra_kv_cache_t *v3_kv = (v3 ? &kv_arr[0] : NULL);
    hydra_kv_cache_t *v4_kvs = (v4 ? kv_arr : NULL);

    double t0 = now_ms();
    for (long s = 0; s < steps; ++s) {
        uint16_t next = 0;
        int step_rc;
        if (sampling_enabled) {
            /* Recent window = the last penalty_window generated tokens
             * (tokens[0..s-1]); the start token is included once it has
             * been superseded by a generated token. v3 passes the same
             * KV cache so the sampled advance keeps the memory channel
             * consistent with the greedy path. */
            size_t nrec = (size_t)s;
            if (nrec > sampler.penalty_window) nrec = sampler.penalty_window;
            step_rc = hydra_sample_token(&engine, current, &sampler,
                                         tokens + (s - nrec), nrec,
                                         v3_kv, v4_kvs, &next);
        } else if (v4) {
            step_rc = hydra_transformer_forward_step(&engine, current, &next, v4_kvs);
        } else if (v3) {
            step_rc = hydra_engine_step_tf(&engine, current, &next, v3_kv);
        } else if (fast_mode) {
            step_rc = hydra_engine_step_fast(&engine, current, &next);
        } else {
            step_rc = hydra_engine_step(&engine, current, &next);
        }
        if (step_rc != 0) {
            free(kv_k); free(kv_v); free(kv_arr);
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
        if (sampler_set || sampling_enabled) {
            printf("[Sampler]: temp=%.4g min-p=%.4g rep-penalty=%.4g rep-window=%u seed=%u%s\n",
                   (double)sampler.temperature, (double)sampler.min_p,
                   (double)sampler.repetition_penalty,
                   (unsigned)sampler.penalty_window, (unsigned)sampler.rng_seed,
                   sampling_enabled ? "" : " (greedy guard aktiv)");
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

    free(kv_k);
    free(kv_v);
    free(kv_arr);
    free(tokens);
    hydra_engine_unload(&engine);
    return 0;
}
