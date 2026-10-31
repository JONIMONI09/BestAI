/* Hydra Unit Tests - JNI token batching
 *
 * Tests the batching policy in android/app/src/main/cpp/hydra_batch.h, which
 * is the very same header the APK compiles into libhydra.so. The loop that
 * decides when a block of tokens crosses into Kotlin is driven here with
 * fake step and emit callbacks, so the behaviour is executable in CI instead
 * of being a claim about a device (no device or emulator is available here -
 * see .agents/docs/gpu-feasibility.md).
 *
 * What is protected:
 *   - a step count that is NOT a multiple of HYDRA_JNI_BATCH still emits its
 *     last, partial block, with done=true (steps=1 and steps=17 are the
 *     cases named in the task),
 *   - done=true happens exactly once per run, on the final block,
 *   - no empty trailing block,
 *   - steps <= 0 is rejected before the engine is stepped at all.
 *
 * Build: make jni-batch-test | Run: ./jni-batch-test
 */
#include "../android/app/src/main/cpp/hydra_batch.h"
#include "../android/app/src/main/cpp/hydra_sample_cfg.h"
#include "hydra_model.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_BLOCKS 64

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    ++tests_run; \
    if (!(cond)) { \
        ++tests_failed; \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

typedef struct {
    int calls;
    int sizes[MAX_BLOCKS];
    int done[MAX_BLOCKS];
    int total;
    int done_count;
    int emit_fail_after; /* -1 = never fail; otherwise fail once this many calls succeeded */
    int seen;
} Emitter;

typedef struct {
    int calls;
    int fail_after; /* -1 = never fail */
} Stepper;

/* 1,2,3,... so a dropped or duplicated token is visible in the sizes. */
static int fake_step(void *user, uint16_t *token_out)
{
    Stepper *s = (Stepper *)user;
    if (s->fail_after >= 0 && s->calls >= s->fail_after) return -1;
    s->calls++;
    *token_out = (uint16_t)((s->calls % 65535u) + 1u);
    return 0;
}

static int fake_emit(void *user, const uint16_t *tokens, int n, int done)
{
    Emitter *e = (Emitter *)user;
    (void)tokens;
    if (e->emit_fail_after >= 0 && e->seen >= e->emit_fail_after) return -1;
    if (e->calls < MAX_BLOCKS) {
        e->sizes[e->calls] = n;
        e->done[e->calls] = done;
    }
    e->calls++;
    e->seen++;
    e->total += n;
    if (done) e->done_count++;
    return 0;
}

static void emitter_reset(Emitter *e)
{
    memset(e, 0, sizeof(*e));
    e->emit_fail_after = -1;
}

static void stepper_reset(Stepper *s)
{
    memset(s, 0, sizeof(*s));
    s->fail_after = -1;
}

static int run_ok(int steps, Emitter *e, Stepper *s, int *blocks, int *total)
{
    HydraBatch b;
    stepper_reset(s);
    emitter_reset(e);
    return hydra_run_steps(&b, steps, fake_step, s, fake_emit, e, blocks, total);
}

/* A run is only correct if these hold, whatever the step count is. */
static int invariants_hold(Emitter *e, int steps, int blocks, int total)
{
    int ok = 1;
    if (total != steps) {
        fprintf(stderr, "  reported %d tokens for %d steps\n", total, steps);
        ok = 0;
    }
    if (e->total != steps) {
        fprintf(stderr, "  emit callback saw %d tokens\n", e->total);
        ok = 0;
    }
    /* Exactly one block - the last - carries done=true. */
    if (blocks != e->calls || e->done_count != 1) {
        fprintf(stderr, "  %d blocks reported, %d emitted, %d with done=true\n",
                blocks, e->calls, e->done_count);
        ok = 0;
    }
    for (int i = 0; i < e->calls && i < MAX_BLOCKS; i++) {
        const int expected_done = (i == e->calls - 1);
        if (e->done[i] != expected_done) {
            fprintf(stderr, "  block %d: done=%d, expected %d\n",
                    i, e->done[i], expected_done);
            ok = 0;
        }
        if (e->sizes[i] <= 0 || e->sizes[i] > HYDRA_JNI_BATCH) {
            fprintf(stderr, "  block %d carried %d tokens (max %d)\n",
                    i, e->sizes[i], HYDRA_JNI_BATCH);
            ok = 0;
        }
    }
    return ok;
}

static void test_explicit_block_shapes(void)
{
    /* steps=1 and steps=17 are the two cases the task names; the rest are
     * the boundaries around the batch size. */
    struct { int steps; int blocks; int first; int last; } cases[] = {
        {  1, 1,  1,  1 },
        {  2, 1,  2,  2 },
        { 15, 1, 15, 15 },
        { 16, 1, 16, 16 },
        { 17, 2, 16,  1 },
        { 31, 2, 16, 15 },
        { 32, 2, 16, 16 },
        { 33, 3, 16,  1 },
    };
    const int n = (int)(sizeof(cases) / sizeof(cases[0]));

    for (int i = 0; i < n; i++) {
        Emitter e;
        Stepper s;
        int blocks = 0, total = 0;
        const int steps = cases[i].steps;

        CHECK(run_ok(steps, &e, &s, &blocks, &total) == HYDRA_RUN_OK,
              "run of the expected step count succeeds");
        CHECK(blocks == cases[i].blocks, "block count matches the shape");
        CHECK(e.calls == cases[i].blocks, "one emit call per block");
        CHECK(e.sizes[0] == cases[i].first, "first block carries the full batch");
        CHECK(e.sizes[e.calls - 1] == cases[i].last,
              "last block carries the remainder, including a partial one");
        CHECK(e.done[e.calls - 1] == 1, "the last block carries done=true");
        CHECK(e.done_count == 1, "done=true exactly once per run");
        CHECK(invariants_hold(&e, steps, blocks, total),
              "all token and done invariants hold");
    }
    printf("  (%d step counts x 7 checks)\n", n);
}

static void test_step_count_sweep(void)
{
    /* The explicit cases can be satisfied by special-casing; a sweep makes
     * that impossible. */
    int all_ok = 1;
    const int max_steps = 4 * HYDRA_JNI_BATCH + 5;

    for (int steps = 1; steps <= max_steps; steps++) {
        Emitter e;
        Stepper s;
        int blocks = 0, total = 0;
        const int expected_blocks = (steps + HYDRA_JNI_BATCH - 1) / HYDRA_JNI_BATCH;

        if (run_ok(steps, &e, &s, &blocks, &total) != HYDRA_RUN_OK
            || blocks != expected_blocks
            || !invariants_hold(&e, steps, blocks, total)) {
            fprintf(stderr, "  first failure at steps=%d\n", steps);
            all_ok = 0;
            break;
        }
    }
    CHECK(all_ok, "every step count from 1 to 4.5 batches emits correct blocks");
}

static void test_non_positive_steps_are_rejected(void)
{
    /* steps <= 0 must be refused BEFORE the engine runs: a zero-step run
     * would otherwise answer ok with no tokens at all. */
    for (int steps = 0; steps >= -3; steps--) {
        Emitter e;
        Stepper s;
        HydraBatch b;
        int blocks = -1, total = -1;

        stepper_reset(&s);
        emitter_reset(&e);
        CHECK(hydra_run_steps(&b, steps, fake_step, &s, fake_emit, &e, &blocks, &total)
                  == HYDRA_RUN_BAD_STEPS,
              "steps <= 0 is rejected");
        CHECK(s.calls == 0, "a rejected run never steps the engine");
        CHECK(e.calls == 0, "a rejected run never emits a block");
        CHECK(blocks == -1 && total == -1,
              "a rejected run writes no output counters");
    }
}

static void test_failures_propagate(void)
{
    HydraBatch b;
    Emitter e;
    Stepper s;
    int blocks = 0, total = 0;

    stepper_reset(&s);
    emitter_reset(&e);
    s.fail_after = 5; /* the sixth step refuses */
    CHECK(hydra_run_steps(&b, 32, fake_step, &s, fake_emit, &e, &blocks, &total)
              == HYDRA_RUN_STEP_ERR,
          "a failing engine step aborts the run");

    stepper_reset(&s);
    emitter_reset(&e);
    e.emit_fail_after = 0; /* even the first block refuses */
    CHECK(hydra_run_steps(&b, 32, fake_step, &s, fake_emit, &e, &blocks, &total)
              == HYDRA_RUN_EMIT_ERR,
          "a failing callback aborts the run");
    CHECK(e.done_count == 0, "an aborted run never reports done");
}

static void test_negative_control_short_tail_is_lost(void)
{
    /* The negative control the positive cases need: without a flush on the
     * last step, steps=17 emits 16 tokens and silently drops the 17th.
     * Measured here, so "the partial block is emitted" is not just an
     * assertion about an implementation nobody runs. */
    HydraBatch b;
    Emitter e;
    Stepper s;
    int blocks = 0, dropped;

    stepper_reset(&s);
    emitter_reset(&e);
    hydra_batch_reset(&b);

    for (int i = 0; i < 17; i++) {
        uint16_t tok = 0;
        fake_step(&s, &tok);
        b.tokens[b.n++] = tok;
        if (b.n >= HYDRA_JNI_BATCH) { /* the bug: no is_last flush */
            fake_emit(&e, b.tokens, b.n, 0);
            b.batches++;
            b.n = 0;
        }
    }
    blocks = b.batches;
    dropped = 17 - e.total;

    CHECK(blocks == 1, "the un-flushed policy emits a single block for steps=17");
    CHECK(dropped == 1, "the un-flushed policy silently drops the 17th token");
}

/* ====================================================================
 * Sampler-configuration validation (hydra_sample_cfg.h).
 *
 * The SAME header the APK compiles into libhydra.so (hydra_jni.c
 * runInferenceSampled): the rules that decide whether a sampling run is
 * accepted at all - which parameter is out of range, which one is
 * reported first, and what "greedy default" means - are executable on
 * the host here, which makes them a test instead of a claim about a
 * device:
 *   - the boundaries (temp 0 and 20, min-p 0 and 1, penalty 1.0) are
 *     ACCEPTED: 0 and 1.0 are valid greedy values, not errors (the
 *     engine's M2 greedy guard, src/hydra_engine.c),
 *   - the FIRST invalid parameter in the order temperature, min-p,
 *     repetition penalty wins (the contract order the pre-extraction
 *     inline code used - the error message a given request receives is
 *     pinned),
 *   - non-finite values are rejected per parameter, including +inf for
 *     the penalty (isfinite, not merely ">= 1"),
 *   - the greedy defaults are pinned to (0, 0, 1.0) and pass
 *     validation; the range constants are pinned so a "harmless"
 *     re-tuning turns red here before it can change what a run means.
 * ==================================================================== */
static void test_sampler_cfg_boundaries(void)
{
    CHECK(hydra_sample_cfg_validate(0.0f, 0.0f, 1.0f) == HYDRA_SAMPLE_OK,
          "the greedy default (0, 0, 1.0) is valid");
    CHECK(hydra_sample_cfg_validate(0.5f, 0.25f, 1.5f) == HYDRA_SAMPLE_OK,
          "a typical sampling configuration is valid");
    CHECK(hydra_sample_cfg_validate(20.0f, 1.0f, 2.0f) == HYDRA_SAMPLE_OK,
          "the upper bounds (temp 20, min-p 1, penalty 2) are included");
    CHECK(hydra_sample_cfg_validate(0.0f, 1.0f, 1.2f) == HYDRA_SAMPLE_OK,
          "min-p = 1 is a valid greedy value, not an error");
}

static void test_sampler_cfg_rejections(void)
{
    CHECK(hydra_sample_cfg_validate(-0.1f, 0.0f, 1.0f) == HYDRA_SAMPLE_BAD_TEMP,
          "a negative temperature is rejected as BAD_TEMP");
    CHECK(hydra_sample_cfg_validate(20.0001f, 0.0f, 1.0f) == HYDRA_SAMPLE_BAD_TEMP,
          "a temperature above 20 is rejected as BAD_TEMP");
    CHECK(hydra_sample_cfg_validate(-1.0f, 2.0f, 1.0f) == HYDRA_SAMPLE_BAD_TEMP,
          "the temperature error wins when later fields are invalid too");
    CHECK(hydra_sample_cfg_validate(0.5f, -0.1f, 1.0f) == HYDRA_SAMPLE_BAD_MINP,
          "a negative min-p is rejected as BAD_MINP");
    CHECK(hydra_sample_cfg_validate(0.5f, 1.0001f, 1.0f) == HYDRA_SAMPLE_BAD_MINP,
          "a min-p above 1 is rejected as BAD_MINP");
    CHECK(hydra_sample_cfg_validate(0.5f, 0.5f, 1.0f) == HYDRA_SAMPLE_OK,
          "penalty 1.0 is the valid 'off' value");
    CHECK(hydra_sample_cfg_validate(0.5f, 0.5f, 0.9999f) == HYDRA_SAMPLE_BAD_REP,
          "a penalty below 1.0 is rejected as BAD_REP");

    const float nan = NAN;
    const float inf = INFINITY;
    CHECK(hydra_sample_cfg_validate(nan, 0.0f, 1.0f) == HYDRA_SAMPLE_BAD_TEMP,
          "a NaN temperature is rejected");
    CHECK(hydra_sample_cfg_validate(inf, 0.0f, 1.0f) == HYDRA_SAMPLE_BAD_TEMP,
          "an infinite temperature is rejected");
    CHECK(hydra_sample_cfg_validate(-inf, 0.0f, 1.0f) == HYDRA_SAMPLE_BAD_TEMP,
          "a -infinite temperature is rejected");
    CHECK(hydra_sample_cfg_validate(0.0f, nan, 1.0f) == HYDRA_SAMPLE_BAD_MINP,
          "a NaN min-p is rejected");
    CHECK(hydra_sample_cfg_validate(0.0f, inf, 1.0f) == HYDRA_SAMPLE_BAD_MINP,
          "an infinite min-p is rejected");
    CHECK(hydra_sample_cfg_validate(0.0f, 0.0f, nan) == HYDRA_SAMPLE_BAD_REP,
          "a NaN penalty is rejected");
    CHECK(hydra_sample_cfg_validate(0.0f, 0.0f, inf) == HYDRA_SAMPLE_BAD_REP,
          "an infinite penalty is rejected (isfinite, not merely >= 1)");
}

static void test_sampler_cfg_defaults_are_pinned(void)
{
    CHECK(HYDRA_SAMPLE_DEFAULT_TEMP == 0.0f &&
              HYDRA_SAMPLE_DEFAULT_MINP == 0.0f &&
              HYDRA_SAMPLE_DEFAULT_REP == 1.0f,
          "the greedy defaults are pinned to (0, 0, 1.0)");
    CHECK(hydra_sample_cfg_validate(HYDRA_SAMPLE_DEFAULT_TEMP,
                                    HYDRA_SAMPLE_DEFAULT_MINP,
                                    HYDRA_SAMPLE_DEFAULT_REP) == HYDRA_SAMPLE_OK,
          "the pinned defaults pass validation");
    CHECK(HYDRA_SAMPLE_TEMP_MIN == 0.0f &&
              HYDRA_SAMPLE_TEMP_MAX - HYDRA_SAMPLE_TEMP_MIN == 20.0f,
          "the temperature range is pinned to [0, 20]");
    CHECK(HYDRA_SAMPLE_MINP_MIN == 0.0f && HYDRA_SAMPLE_MINP_MAX == 1.0f,
          "the min-p range is pinned to [0, 1]");
    CHECK(HYDRA_SAMPLE_REP_MIN == 1.0f,
          "the penalty lower bound is pinned to 1.0");
}

/* ====================================================================
 * Transformer (v3/v4) dispatch through the JNI contract
 * ====================================================================
 * Java_dev_hydrastone_HydraBridge_runInferenceSampled is a THIN wrapper
 * around three pieces that are testable on the host without a JVM:
 *   1. hydra_sample_cfg_validate() (the same header, covered above),
 *   2. the KV-cache geometry (hydra_jni.c: v3 -> one shared ring,
 *      v4 -> one ring PER LAYER, cap = steps + 4, one sink),
 *   3. hydra_run_steps() driving the engine's own dispatch
 *      hydra_sampled_step() (src/hydra_engine.c: v1/v2 -> native step,
 *      v3/v4 -> tf path, sampling on -> hydra_sample_token()).
 *
 * This section drives 2 + 3 with REAL models: deterministic v3/v4
 * writers (same layout and LCG as tests/test_engine.c, copied to keep
 * this binary self-contained) plus the exact allocation contract of
 * the JNI wrapper. The load-bearing property: the BATCHED run (what
 * the phone executes, blocks of HYDRA_JNI_BATCH crossing into Kotlin)
 * must produce the token stream of the DIRECT run (same engine load,
 * same KV, same step loop without batching) - the batching policy
 * may shape blocks, it must never shape tokens. The greedy stream is
 * additionally pinned, so a regression in the tf path cannot hide
 * behind "batch equals direct".
 * ==================================================================== */

#define V3V4_STEPS_MAX 64
#define V3V4_VOCAB 33u
/* Pinned below after the first measured run (see the pinning CHECK). */
#define V3V4_V4_GREEDY_T0 15u
#define V3V4_V4_GREEDY_T1 12u
#define V3V4_V4_GREEDY_T2 12u

/* LCG + packing: identical to tests/test_engine.c (lcg_byte, v3_pack). */
static uint32_t jb_lcg_state;
static void jb_lcg_seed(uint32_t s) { jb_lcg_state = s; }
static uint8_t jb_lcg_byte(void)
{
    jb_lcg_state = jb_lcg_state * 1103515245u + 12345u;
    return (uint8_t)(jb_lcg_state >> 16);
}
static void jb_v3_pack(uint8_t *bytep, uint32_t j, int code)
{
    *bytep |= (uint8_t)((code & 0x03u) << (2 * (j & 3u)));
}
static void jb_v3_fill_block(uint8_t *blk, uint32_t dim, uint32_t rows,
                             uint32_t cols)
{
    for (uint32_t i = 0; i < rows; ++i)
        for (uint32_t j = 0; j < cols; ++j)
            jb_v3_pack(blk + (size_t)i * (dim / 4) + (size_t)(j / 4), j,
                       jb_lcg_byte() % 3);
}
static void jb_wr_u16le(FILE *f, uint16_t v)
{
    fputc(v & 0xff, f); fputc(v >> 8, f);
}
static void jb_wr_u32le(FILE *f, uint32_t v)
{
    fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
    fputc((v >> 16) & 0xff, f); fputc(v >> 24, f);
}
static void jb_wr_header_version(FILE *f, uint16_t version, uint16_t vocab,
                                 uint32_t dim, uint32_t layers, uint32_t off,
                                 uint32_t wlen)
{
    jb_wr_u32le(f, HYDRA_MAGIC);
    jb_wr_u16le(f, version);
    jb_wr_u16le(f, vocab);
    jb_wr_u32le(f, dim);
    jb_wr_u32le(f, layers);
    jb_wr_u32le(f, off);
    jb_wr_u32le(f, wlen);
}

/* v3 layout (docs/FORMAT.md "Version 3"): per layer
 * Wg | Wu | w1 | u16 scale, plus the final W_out. */
static void jb_write_v3_model(const char *path, uint32_t dim, uint32_t layers,
                              uint16_t vocab, uint32_t seed)
{
    jb_lcg_seed(seed);
    const size_t lb = (size_t)dim * dim / 2 + dim / 4 + 2;
    const size_t body_len = (size_t)layers * lb + (size_t)dim * dim / 4;
    uint8_t *body = (uint8_t *)malloc(body_len);
    if (!body) { perror("malloc"); exit(1); }
    memset(body, 0, body_len);
    for (uint32_t l = 0; l < layers; ++l) {
        uint8_t *base = body + (size_t)l * lb;
        jb_v3_fill_block(base, dim, dim, dim);
        jb_v3_fill_block(base + (size_t)dim * dim / 4, dim, dim, dim);
        jb_v3_fill_block(base + (size_t)dim * dim / 2, dim, 1, dim);
        base[lb - 2] = 0; /* scale = 256 (absmean 1.0, fixed-point 256) */
        base[lb - 1] = 1;
    }
    jb_v3_fill_block(body + (size_t)layers * lb, dim, dim, dim);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) { perror("open"); exit(1); }
    FILE *f = fdopen(fd, "wb");
    if (!f) { perror("fdopen"); close(fd); exit(1); }
    jb_wr_header_version(f, 3, vocab, dim, layers,
                         (uint32_t)sizeof(HydraModelHeader),
                         (uint32_t)body_len);
    fwrite(body, 1, body_len, f);
    fclose(f);
    free(body);
}

/* v4 layout (docs/FORMAT.md "Version 4"): per layer
 * Wg | Wu | Wq | Wk | Wv | w1 | u16 scale, plus the final W_out. */
static void jb_write_v4_model(const char *path, uint32_t dim, uint32_t layers,
                              uint16_t vocab, uint32_t seed)
{
    jb_lcg_seed(seed);
    const size_t lb = 5u * (size_t)dim * dim / 4u + (size_t)dim / 4u + 2;
    const size_t body_len = (size_t)layers * lb + (size_t)dim * dim / 4;
    uint8_t *body = (uint8_t *)calloc(1, body_len);
    if (!body) { perror("calloc"); exit(1); }
    for (uint32_t l = 0; l < layers; ++l) {
        uint8_t *base = body + (size_t)l * lb;
        for (int b = 0; b < 5; ++b)
            jb_v3_fill_block(base + (size_t)b * (size_t)dim * dim / 4,
                             dim, dim, dim);
        jb_v3_fill_block(base + 5 * (size_t)dim * dim / 4, dim, 1, dim);
        base[lb - 2] = 0; /* scale = 256 */
        base[lb - 1] = 1;
    }
    jb_v3_fill_block(body + (size_t)layers * lb, dim, dim, dim);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) { perror("open"); exit(1); }
    FILE *f = fdopen(fd, "wb");
    if (!f) { perror("fdopen"); close(fd); exit(1); }
    jb_wr_header_version(f, 4, vocab, dim, layers,
                         (uint32_t)sizeof(HydraModelHeader),
                         (uint32_t)body_len);
    fwrite(body, 1, body_len, f);
    fclose(f);
    free(body);
}

/* Emits the same blocks as the JNI emit callback but RECORDS the
 * tokens, so the produced stream is comparable. */
typedef struct {
    int calls;
    int sizes[MAX_BLOCKS];
    int done[MAX_BLOCKS];
    int total;
    int done_count;
    uint16_t tokens[V3V4_STEPS_MAX];
    int ntok;
} RecEmitter;

static int rec_emit(void *user, const uint16_t *tokens, int n, int done)
{
    RecEmitter *e = (RecEmitter *)user;
    if (e->calls < MAX_BLOCKS) {
        e->sizes[e->calls] = n;
        e->done[e->calls] = done;
    }
    e->calls++;
    e->total += n;
    if (done) e->done_count++;
    if (e->ntok + n <= V3V4_STEPS_MAX) {
        memcpy(e->tokens + e->ntok, tokens, (size_t)n * sizeof(uint16_t));
        e->ntok += n;
    }
    return 0;
}

/* The exact per-run setup of runInferenceSampled (hydra_jni.c):
 * n_kv = v4 ? layers : v3 ? 1 : 0, cap = steps + 4, one sink,
 * per-cache stride (cap + sink) * dim floats, gen buffer of steps.
 * Returns 0 on success and fills *stream (capacity steps),
 * *blocks_out (when non-NULL) with the emitted block count. */
static int jb_run_transformer(uint16_t version, const char *path, int steps,
                              float temp, float minp, float rep, uint32_t seed,
                              const uint16_t *prompt, int prompt_n,
                              uint16_t *stream, int *blocks_out)
{
    HydraEngine engine;
    if (hydra_engine_load(&engine, path) != 0)
        return -1;
    const uint32_t dim = engine.header.dim;
    const uint32_t layers = engine.header.layers;
    const uint32_t n_kv =
        (version == 4) ? layers : ((version == 3) ? 1u : 0u);
    float *kv_k = NULL, *kv_v = NULL;
    hydra_kv_cache_t *kv_arr = NULL, *kv = NULL, *kv_layers = NULL;
    if (n_kv > 0) {
        const uint32_t cap = (uint32_t)steps + 4, nsink = 1;
        const size_t per = ((size_t)cap + nsink) * (size_t)dim * sizeof(float);
        kv_k = (float *)malloc(per * (size_t)n_kv);
        kv_v = (float *)malloc(per * (size_t)n_kv);
        kv_arr = (hydra_kv_cache_t *)calloc(n_kv, sizeof(hydra_kv_cache_t));
        if (!kv_k || !kv_v || !kv_arr) {
            free(kv_k); free(kv_v); free(kv_arr);
            hydra_engine_unload(&engine);
            return -1;
        }
        for (uint32_t l = 0; l < n_kv; ++l) {
            if (hydra_kv_cache_init(&kv_arr[l],
                    kv_k + (size_t)l * ((size_t)cap + nsink) * (size_t)dim,
                    kv_v + (size_t)l * ((size_t)cap + nsink) * (size_t)dim,
                    dim, cap, nsink) != 0) {
                free(kv_k); free(kv_v); free(kv_arr);
                hydra_engine_unload(&engine);
                return -1;
            }
        }
        kv = (version == 3) ? &kv_arr[0] : NULL;
        kv_layers = (version == 4) ? kv_arr : NULL;
    }
    uint16_t *gen = (uint16_t *)malloc((size_t)steps * sizeof(uint16_t));
    if (!gen) {
        free(kv_k); free(kv_v); free(kv_arr);
        hydra_engine_unload(&engine);
        return -1;
    }

    hydra_sampled_step_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* Same prompt semantics as the wrapper: prefill the whole prompt,
     * the LAST token is the generation seed, out-of-vocab values are
     * modularised, not rejected. */
    if (prompt_n > 0 &&
        hydra_engine_prefill(&engine, prompt, (size_t)prompt_n) != 0) {
        free(gen); free(kv_k); free(kv_v); free(kv_arr);
        hydra_engine_unload(&engine);
        return -1;
    }
    uint16_t seed_tok = prompt_n > 0 ? prompt[prompt_n - 1] : 0u;
    if (seed_tok >= engine.header.vocab_size)
        seed_tok = (uint16_t)(seed_tok % engine.header.vocab_size);
    ctx.engine = &engine;
    ctx.tok = seed_tok;
    ctx.cfg.temperature = temp;
    ctx.cfg.min_p = minp;
    ctx.cfg.repetition_penalty = rep;
    ctx.cfg.penalty_window = 64; /* the wrapper's fixed window */
    ctx.cfg.rng_seed = seed;
    ctx.gen = gen;
    ctx.s = 0;
    ctx.kv = kv;
    ctx.kv_layers = kv_layers;

    RecEmitter e;
    memset(&e, 0, sizeof(e));
    HydraBatch batch;
    int blocks = 0, total = 0;
    int rc = hydra_run_steps(&batch, steps, hydra_sampled_step, &ctx,
                             rec_emit, &e, &blocks, &total);
    const int ok =
        (rc == HYDRA_RUN_OK) && (e.ntok == steps) && (total == steps) &&
        (e.done_count == 1) && (e.calls == blocks);
    if (ok)
        memcpy(stream, e.tokens, (size_t)steps * sizeof(uint16_t));
    if (blocks_out) *blocks_out = blocks;
    free(gen);
    free(kv_k); free(kv_v); free(kv_arr);
    hydra_engine_unload(&engine);
    return ok ? 0 : -2;
}

/* The same run WITHOUT batching: the step callback called directly,
 * step for step. This is the reference the batched run must match. */
static int jb_run_transformer_direct(uint16_t version, const char *path,
                                     int steps, float temp, float minp,
                                     float rep, uint32_t seed,
                                     const uint16_t *prompt, int prompt_n,
                                     uint16_t *stream)
{
    HydraEngine engine;
    if (hydra_engine_load(&engine, path) != 0)
        return -1;
    const uint32_t dim = engine.header.dim;
    const uint32_t layers = engine.header.layers;
    const uint32_t n_kv =
        (version == 4) ? layers : ((version == 3) ? 1u : 0u);
    float *kv_k = NULL, *kv_v = NULL;
    hydra_kv_cache_t *kv_arr = NULL, *kv = NULL, *kv_layers = NULL;
    if (n_kv > 0) {
        const uint32_t cap = (uint32_t)steps + 4, nsink = 1;
        const size_t per = ((size_t)cap + nsink) * (size_t)dim * sizeof(float);
        kv_k = (float *)malloc(per * (size_t)n_kv);
        kv_v = (float *)malloc(per * (size_t)n_kv);
        kv_arr = (hydra_kv_cache_t *)calloc(n_kv, sizeof(hydra_kv_cache_t));
        if (!kv_k || !kv_v || !kv_arr) {
            free(kv_k); free(kv_v); free(kv_arr);
            hydra_engine_unload(&engine);
            return -1;
        }
        for (uint32_t l = 0; l < n_kv; ++l) {
            if (hydra_kv_cache_init(&kv_arr[l],
                    kv_k + (size_t)l * ((size_t)cap + nsink) * (size_t)dim,
                    kv_v + (size_t)l * ((size_t)cap + nsink) * (size_t)dim,
                    dim, cap, nsink) != 0) {
                free(kv_k); free(kv_v); free(kv_arr);
                hydra_engine_unload(&engine);
                return -1;
            }
        }
        kv = (version == 3) ? &kv_arr[0] : NULL;
        kv_layers = (version == 4) ? kv_arr : NULL;
    }
    uint16_t *gen = (uint16_t *)malloc((size_t)steps * sizeof(uint16_t));
    if (!gen) {
        free(kv_k); free(kv_v); free(kv_arr);
        hydra_engine_unload(&engine);
        return -1;
    }
    hydra_sampled_step_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* Mirror of the wrapper's prompt semantics (see the batched
     * runner): prefill, last token = seed, modularised. */
    if (prompt_n > 0 &&
        hydra_engine_prefill(&engine, prompt, (size_t)prompt_n) != 0) {
        free(gen); free(kv_k); free(kv_v); free(kv_arr);
        hydra_engine_unload(&engine);
        return -1;
    }
    uint16_t seed_tok = prompt_n > 0 ? prompt[prompt_n - 1] : 0u;
    if (seed_tok >= engine.header.vocab_size)
        seed_tok = (uint16_t)(seed_tok % engine.header.vocab_size);
    ctx.engine = &engine;
    ctx.tok = seed_tok;
    ctx.cfg.temperature = temp;
    ctx.cfg.min_p = minp;
    ctx.cfg.repetition_penalty = rep;
    ctx.cfg.penalty_window = 64;
    ctx.cfg.rng_seed = seed;
    ctx.gen = gen;
    ctx.s = 0;
    ctx.kv = kv;
    ctx.kv_layers = kv_layers;

    int ok = 1;
    for (int s = 0; s < steps && ok; ++s) {
        uint16_t tok = 0;
        if (hydra_sampled_step(&ctx, &tok) != 0) ok = 0;
        stream[s] = tok;
    }
    free(gen);
    free(kv_k); free(kv_v); free(kv_arr);
    hydra_engine_unload(&engine);
    return ok ? 0 : -2;
}

static int jb_streams_equal(const uint16_t *a, const uint16_t *b, int n)
{
    for (int i = 0; i < n; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}

static void test_transformer_v3_v4_jni_contract(void)
{
    const uint32_t dim = 16, layers = 2;
    char p3[80], p4[80];
    snprintf(p3, sizeof p3, ".jb-v3-%d.hydra", (int)getpid());
    snprintf(p4, sizeof p4, ".jb-v4-%d.hydra", (int)getpid());
    jb_write_v3_model(p3, dim, layers, V3V4_VOCAB, 0x30C1u);
    jb_write_v4_model(p4, dim, layers, V3V4_VOCAB, 0x41u);

    uint16_t a[V3V4_STEPS_MAX], b[V3V4_STEPS_MAX];
    const uint16_t p1[1] = { 7u };
    const uint16_t p3p[3] = { 7u, 11u, 13u };
    const int steps = 10;

    /* v3 greedy: the batched JNI run must equal the direct run. */
    CHECK(jb_run_transformer_direct(3, p3, steps, 0.0f, 0.0f, 1.0f, 777,
                                    p3p, 3, a) == 0,
          "v3: direct step loop runs (3-token prompt)");
    int blocks = 0;
    CHECK(jb_run_transformer(3, p3, steps, 0.0f, 0.0f, 1.0f, 777, p3p, 3,
                             b, &blocks) == 0,
          "v3: batched JNI-contract run emits its invariants");
    CHECK(jb_streams_equal(a, b, steps),
          "v3: batched run == direct run (greedy) - batching shapes "
          "blocks, never tokens");
    CHECK(blocks == 1, "v3: 10 steps fit one 16-token block (done=true once)");

    /* v4 greedy: same contract with the per-layer cache array. */
    CHECK(jb_run_transformer_direct(4, p4, steps, 0.0f, 0.0f, 1.0f, 777,
                                    p3p, 3, a) == 0,
          "v4: direct step loop runs (3-token prompt)");
    CHECK(jb_run_transformer(4, p4, steps, 0.0f, 0.0f, 1.0f, 777, p3p, 3,
                             b, &blocks) == 0,
          "v4: batched JNI-contract run emits its invariants");
    CHECK(jb_streams_equal(a, b, steps),
          "v4: batched run == direct run (greedy, per-layer KV)");

    /* v4 token stream is pinned (measured 2026-10-27, prompt 7,11,13,
     * seed 777, greedy): a regression in the tf path or the writers
     * cannot hide behind "batch equals direct". */
    jb_run_transformer(4, p4, steps, 0.0f, 0.0f, 1.0f, 777, p3p, 3, b, NULL);
    CHECK(b[0] == V3V4_V4_GREEDY_T0 && b[1] == V3V4_V4_GREEDY_T1 &&
              b[2] == V3V4_V4_GREEDY_T2,
          "v4: greedy stream pinned (first 3 tokens, prompt 7,11,13)");

    /* v3 sampling: deterministic for a fixed seed (the wrapper passes
     * the seed through; 0 maps to the engine fallback, never a
     * degenerate stream). */
    CHECK(jb_run_transformer(3, p3, steps, 1.0f, 0.5f, 1.5f, 4242, p3p, 3,
                             a, NULL) == 0,
          "v3: sampled run (temp 1.0, min-p 0.5, rp 1.5) runs");
    CHECK(jb_run_transformer(3, p3, steps, 1.0f, 0.5f, 1.5f, 4242, p3p, 3,
                             b, NULL) == 0,
          "v3: second sampled run with the same seed runs");
    CHECK(jb_streams_equal(a, b, steps),
          "v3: sampled stream deterministic for a fixed seed");

    /* v4 sampling through the per-layer caches. */
    CHECK(jb_run_transformer(4, p4, steps, 1.5f, 0.25f, 1.2f, 99, p3p, 3,
                             a, NULL) == 0,
          "v4: sampled run runs");
    CHECK(jb_run_transformer(4, p4, steps, 1.5f, 0.25f, 1.2f, 99, p3p, 3,
                             b, NULL) == 0,
          "v4: second sampled run runs");
    CHECK(jb_streams_equal(a, b, steps),
          "v4: sampled stream deterministic for a fixed seed");

    /* Fixed point of the bias-free design: token 0 with the zero start
     * state drives xeff = h + w1*0 = 0, and every layer is a mat-vec
     * over that zero - the whole stream is 0. Guards the "no bias
     * term" property of v3/v4 (an added bias would break this pin). */
    CHECK(jb_run_transformer(4, p4, steps, 0.0f, 0.0f, 1.0f, 1, NULL, 0,
                             b, NULL) == 0,
          "v4: token-0/no-prompt run runs");
    int zero_stream = 1;
    for (int i = 0; i < steps; ++i)
        if (b[i] != 0u) zero_stream = 0;
    CHECK(zero_stream, "v4: token 0 from the zero state is the all-zero "
          "fixed point (bias-free design)");
    CHECK(jb_run_transformer(3, p3, steps, 0.0f, 0.0f, 1.0f, 1, NULL, 0,
                             b, NULL) == 0 && b[0] == 0u && b[5] == 0u,
          "v3: the same fixed point holds (token 0, zero state)");

    /* steps=17 with a REAL step function: the last partial block is
     * emitted with done=true (the fake-step sweep covers the policy;
     * this proves it holds when the step callback is the engine). */
    CHECK(jb_run_transformer(3, p3, 17, 0.0f, 0.0f, 1.0f, 5, p1, 1,
                             a, &blocks) == 0,
          "v3: 17-step run (16 + partial) runs");
    CHECK(blocks == 2, "v3: 17 steps emit exactly two blocks");

    unlink(p3);
    unlink(p4);
}

int main(void)
{
    printf("== JNI batching policy (HYDRA_JNI_BATCH=%d) ==\n", HYDRA_JNI_BATCH);
    test_explicit_block_shapes();
    test_step_count_sweep();
    test_non_positive_steps_are_rejected();
    test_failures_propagate();
    test_negative_control_short_tail_is_lost();
    printf("== sampler configuration (hydra_sample_cfg.h) ==\n");
    test_sampler_cfg_boundaries();
    test_sampler_cfg_rejections();
    test_sampler_cfg_defaults_are_pinned();
    printf("== transformer (v3/v4) through the JNI contract ==\n");
    test_transformer_v3_v4_jni_contract();
    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}