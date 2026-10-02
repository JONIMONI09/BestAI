/* Token batching for the JNI bridge - the ONE place that decides when a
 * block of tokens is handed to Kotlin.
 *
 * Why this is a header and not inline code in hydra_jni.c: the rule that
 * matters ("a run whose step count is not a multiple of the batch size
 * must still emit its last, partial block, with done=true, exactly once")
 * was never executable without a device. Extracting it here lets
 * tests/test_jni_batch.c run the SAME loop on the host with fake step and
 * emit callbacks, which is what makes it a test instead of a claim.
 *
 * The Android build includes this header (see CMakeLists.txt); the host
 * test includes the same file from the repository. There is no second copy
 * of the rule to drift out of sync.
 */
#ifndef HYDRA_BATCH_H
#define HYDRA_BATCH_H

#include <stdint.h>

/* How many tokens are gathered before crossing into Kotlin. The per-token
 * transition (JNI call + Kotlin object dispatch) costs more than the engine
 * step at this model size; 16 keeps progress visible without flooding the
 * UI thread. */
#ifndef HYDRA_JNI_BATCH
#define HYDRA_JNI_BATCH 16
#endif

/* Return codes of hydra_run_steps(). */
#define HYDRA_RUN_OK        0
#define HYDRA_RUN_BAD_STEPS (-1) /* steps < 1: rejected, nothing was stepped */
#define HYDRA_RUN_STEP_ERR  (-2) /* the step callback refused */
#define HYDRA_RUN_EMIT_ERR  (-3) /* the emit callback refused */

typedef struct {
    uint16_t tokens[HYDRA_JNI_BATCH];
    int n;       /* tokens currently buffered */
    int batches; /* emit calls made so far */
} HydraBatch;

/** Produces the next token. Returns 0 on success, anything else aborts. */
typedef int (*hydra_step_fn)(void *user, uint16_t *token_out);

/** Hands a finished block to the UI. done must be true only on the last
 *  call of a run. Returns 0 on success, anything else aborts. */
typedef int (*hydra_emit_fn)(void *user, const uint16_t *tokens, int n, int done);

static inline void hydra_batch_reset(HydraBatch *b)
{
    b->n = 0;
    b->batches = 0;
}

/**
 * A block must leave the native side when it is full, or when it is the
 * last one - including when that last block is short (1..15 tokens) or
 * empty because the step count was an exact multiple of the batch size.
 *
 * Flushing on `last` rather than "flush whatever is left afterwards" is
 * what removes the empty trailing block: with steps=16 exactly, the 16th
 * token already fills a block, so it is emitted with done=true instead of
 * being emitted twice (once as a full block, once as an empty one).
 */
static inline int hydra_batch_should_emit(const HydraBatch *b, int is_last)
{
    if (b->n <= 0) return 0;
    return b->n >= HYDRA_JNI_BATCH || is_last;
}

/**
 * Runs `steps` steps through `step`, buffering tokens and handing them to
 * `emit` in blocks of at most HYDRA_JNI_BATCH.
 *
 * The two callbacks take separate user pointers on purpose: they almost
 * always need different state (the engine handle vs. the UI callback), and
 * one shared pointer invites exactly the type confusion the host test
 * originally had.
 *
 * steps < 1 is rejected BEFORE the first step: a run of zero steps would
 * otherwise report ok with no tokens, and a caller that computed
 * `steps = end - start` on an empty range would get a silent success.
 *
 * Returns HYDRA_RUN_*; on success *blocks holds the number of emit calls
 * and *total the number of tokens emitted.
 */
static inline int hydra_run_steps(HydraBatch *batch, int steps,
                                  hydra_step_fn step, void *step_user,
                                  hydra_emit_fn emit, void *emit_user,
                                  int *blocks, int *total)
{
    uint16_t tok = 0;

    if (steps < 1) return HYDRA_RUN_BAD_STEPS;
    hydra_batch_reset(batch);

    for (int s = 0; s < steps; ++s) {
        const int is_last = (s == steps - 1);
        if (step(step_user, &tok) != 0) return HYDRA_RUN_STEP_ERR;
        batch->tokens[batch->n++] = tok;

        if (hydra_batch_should_emit(batch, is_last)) {
            if (emit(emit_user, batch->tokens, batch->n, is_last) != 0) {
                return HYDRA_RUN_EMIT_ERR;
            }
            batch->batches++;
            batch->n = 0;
        }
    }

    if (blocks) *blocks = batch->batches;
    if (total) *total = steps;
    return HYDRA_RUN_OK;
}

#endif /* HYDRA_BATCH_H */