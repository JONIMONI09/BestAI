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

#include <stdio.h>
#include <string.h>

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
    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}