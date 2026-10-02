/* Hydra NEON Ternary Accumulation Kernel (AArch64 / ARMv7-A + NEON)
 *
 * INTEGRIERTER Pfad: wird von hydra_engine_step() benutzt, wenn
 *   (a) mit __ARM_NEON kompiliert wird und
 *   (b) dim ein Vielfaches von 4 ist (int32-Lanes).
 * Alle anderen Faelle (x86, dim % 4 != 0) nutzen automatisch den
 * skalaren Fallback — identische Semantik, siehe hydra_engine.c.
 *
 * Warum dieser Kernel nicht mehr über Layer läuft:
 * hydra_engine_load() verdichtet die Layer zu zwei Summen
 *   A[i] = sum_l w1[l][i],  B[i] = sum_l w2[l][i]
 * Der heisse Pfad ist damit
 *   acc[i] = A[i]*token + B[i]*state[i]
 * also ein Vector-Mul-Add pro Dimension statt eines Decode+Mul-Add pro
 * Layer. Der frühere Layer-Kernel wäre nach dieser Verdichtung toter Code
 * gewesen — und ein toter SIMD-Pfad ist eine Behauptung, keine
 * Beschleunigung: der ARM-Pfad wäre ungeprüft geblieben.
 *
 * BIT-IDENTISCH-GARANTIE: die Dekodier-Schritte (Codes 00/01/10/11 ->
 * 0/+1/-1/0) passieren jetzt beim Laden in hydra_build_aggregation(),
 * also rein skalar und damit architekturunabhängig. Der Vektor-Pfad
 * unten multiplies nur noch fertige int32-Summen und kann deshalb gar
 * keine Vorzeichen-Interpretation mehr falsch machen. tests/test_engine.c
 * vergleicht diesen Pfad direkt gegen die skalare Referenz
 * (test_neon_matches_scalar, test_neon_all_ternary_codes).
 */
#ifndef HYDRA_NEON_H
#define HYDRA_NEON_H

#ifdef __ARM_NEON
#define HYDRA_USE_NEON 1
#include <arm_neon.h>
#include <stdint.h>

/**
 * acc[i] += A[i]*token + B[i]*state[i]  fuer genau 16 Werte.
 *
 * Feste Breite statt Parameter: der einzige Aufrufer verarbeitet
 * 16er-Chunks, und eine freie Restlaenge ergaebe dem Compiler keinen
 * Beweis fuer die Schleifengrenze ("-Waggressive-loop-optimizations").
 *
 * Rechnung in int32 (|A|,|B| <= layers <= 4096, |token| <= 1023,
 * |state| <= 127 -> |Produkt| <= 4.2e6, weit unter INT32_MAX), danach
 * eine Erweiterung auf int64 fuer den Akkumulator.
 */
static inline void hydra_neon_accumulate_agg(
    int64_t *acc,
    const int32_t *a,
    const int32_t *b,
    const int8_t *state,
    int32_t token_val)
{
    const int32x4_t tok = vdupq_n_s32(token_val);

    for (size_t i = 0; i < 16; i += 4) {
        int32x4_t va = vld1q_s32(a + i);
        int32x4_t vb = vld1q_s32(b + i);
        /* state ist int8_t. vld1q_s8 laedt 16 Byte - das ist hier sicher,
         * weil die Schleife nur laeuft solange i+16 <= n gilt und n nie
         * groesser als dim <= HYDRA_EMBED_DIM (64) ist. vget_low_s16
         * nimmt die unteren 4 Lanes, weil vmovl_s8 auf AArch64 zu 8
         * Lanes promoted wird. */
        int32x4_t vs = vmovl_s16(
            vget_low_s16(vmovl_s8(vget_low_s8(vld1q_s8(state + i)))));
        int32x4_t v = vmlaq_s32(vmulq_s32(va, tok), vb, vs);

        vst1q_s64(acc + i + 0,
                  vaddq_s64(vld1q_s64(acc + i + 0), vmovl_s32(vget_low_s32(v))));
        vst1q_s64(acc + i + 2,
                  vaddq_s64(vld1q_s64(acc + i + 2), vmovl_s32(vget_high_s32(v))));
    }
}

#endif /* __ARM_NEON */
#endif /* HYDRA_NEON_H */