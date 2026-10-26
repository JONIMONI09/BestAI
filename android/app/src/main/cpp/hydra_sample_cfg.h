/* Sampler-configuration validation for the sampled run (the JNI bridge's
 * runInferenceSampled entry point).
 *
 * Why this is a header and not inline code in hydra_jni.c: the rules that
 * decide whether a sampling run is accepted at all (which parameter is out
 * of range, which one is reported first, what "greedy default" means)
 * used to live only inline in the Android .c file - they were never
 * executable without a device. Extracting them here, exactly like the
 * batching rule in hydra_batch.h, lets tests/test_jni_batch.c run the SAME
 * validation on the host. The Android build includes this header (see
 * hydra_jni.c); the host test includes the same file from the repository.
 * There is no second copy of the rule to drift out of sync.
 *
 * The three surfaces that must share these rules are checked by
 * tools/sampler_contract_check.sh:
 *   - the CLI      (src/main.c: --temp/--min-p/--rep-penalty/--rep-window)
 *   - this header  (Android, hydra_jni.c runInferenceSampled)
 *   - the web      (server.js /api/infer)
 * The deliberate difference, pinned by that gate: the CLI is finite-only
 * for --temp (power users type what they mean), while the UI-facing
 * surfaces (this header + server.js) cap temperature at 20.
 */
#ifndef HYDRA_SAMPLE_CFG_H
#define HYDRA_SAMPLE_CFG_H

#include <math.h>

/* Accepted ranges. 0 = greedy: temperature <= 0 or min_p >= 1 both mean
 * "no sampling" in the engine (the M2 greedy guard in src/hydra_engine.c),
 * so 0.0 and 1.0 are VALID, not errors. */
#define HYDRA_SAMPLE_TEMP_MIN   0.0f
#define HYDRA_SAMPLE_TEMP_MAX   20.0f
#define HYDRA_SAMPLE_MINP_MIN   0.0f
#define HYDRA_SAMPLE_MINP_MAX   1.0f
#define HYDRA_SAMPLE_REP_MIN    1.0f

/* Validation results. Which code wins is part of the contract: a request
 * with several invalid fields must always be rejected for the FIRST one
 * in the order temperature, min-p, repetition penalty - the order the
 * inline code used before this extraction, so the error the user sees
 * for a given request cannot change. */
#define HYDRA_SAMPLE_OK         0
#define HYDRA_SAMPLE_BAD_TEMP   1 /* not finite, or outside [0, 20] */
#define HYDRA_SAMPLE_BAD_MINP   2 /* not finite, or outside [0, 1] */
#define HYDRA_SAMPLE_BAD_REP    3 /* not finite, or below 1.0 */

/**
 * Validates one sampled-run configuration.
 *
 * Returns HYDRA_SAMPLE_OK when all three parameters are finite and inside
 * their ranges, otherwise the code of the offending parameter (checked in
 * the contract order above). No output parameters on purpose: the caller
 * already holds the three floats, the value is the range proof itself -
 * an out parameter would be a second, drift-able copy of the input.
 */
static inline int hydra_sample_cfg_validate(float temperature, float min_p,
                                            float rep_penalty)
{
    if (!isfinite(temperature) || temperature < HYDRA_SAMPLE_TEMP_MIN ||
        temperature > HYDRA_SAMPLE_TEMP_MAX)
        return HYDRA_SAMPLE_BAD_TEMP;
    if (!isfinite(min_p) || min_p < HYDRA_SAMPLE_MINP_MIN ||
        min_p > HYDRA_SAMPLE_MINP_MAX)
        return HYDRA_SAMPLE_BAD_MINP;
    if (!isfinite(rep_penalty) || rep_penalty < HYDRA_SAMPLE_REP_MIN)
        return HYDRA_SAMPLE_BAD_REP;
    return HYDRA_SAMPLE_OK;
}

/* Greedy defaults: a plain invocation (no sampler flags) must stay
 * bit-identical to the pre-sampling engine, and these three constants are
 * what the CLI (src/main.c) and the web console (server.js, when the
 * client sends nothing) both fall back to. They are pinned, not merely
 * "typical": temp 0 is the greedy guard, min_p 0 disables the filter,
 * penalty 1.0 is "no discounting" (a penalty < 1 would BOOST repeated
 * tokens - the one value that is accepted but never used). */
#define HYDRA_SAMPLE_DEFAULT_TEMP 0.0f
#define HYDRA_SAMPLE_DEFAULT_MINP 0.0f
#define HYDRA_SAMPLE_DEFAULT_REP  1.0f

#endif /* HYDRA_SAMPLE_CFG_H */
