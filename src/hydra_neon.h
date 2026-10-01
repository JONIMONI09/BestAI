/* Hydra NEON Ternary Accumulation Kernel (AArch64 / ARMv7-A + NEON)
 *
 * INTEGRIERTER Pfad: wird von hydra_engine_step() benutzt, wenn
 *   (a) mit __ARM_NEON kompiliert wird und
 *   (b) dim ein Vielfaches von 16 ist.
 * Alle anderen Faelle (x86, dim % 16 != 0) nutzen automatisch den
 * skalaren Fallback — identische Semantik, siehe hydra_engine.c.
 *
 * Layout entspricht exakt dem .hydra-Packing (docs/FORMAT.md):
 * 2 ternare Gewichte pro Byte, w1 = Bits 0-1, w2 = Bits 2-3.
 *
 * Verifikation: Der NEON-Pfad laeuft in der CI auf macOS-ARM64-Runnern
 * und wird dort von den Seed-Roundtrip-Tests (32 Steps, Reproduzierbarkeit)
 * vollstaendig durchlaufen. x86-CI deckt den skalaren Pfad ab.
 */
#ifndef HYDRA_NEON_H
#define HYDRA_NEON_H

#ifdef __ARM_NEON
#define HYDRA_USE_NEON 1
#include <arm_neon.h>
#include <stdint.h>

/**
 * Akkumuliert einen 16-Lane-Chunk ternarer Produkte:
 *   acc[i] += w1(packed[i]) * token + w2(packed[i]) * state[i]
 *
 * @param packed    16 Bytes im Format- Packing (2 Gewichte/Byte)
 * @param state     16 int8 State-Werte (Layer-Eingang)
 * @param token_val Token-Wert (Bereich 0..127, siehe step())
 * @param acc       16 int32-Akkumulatoren, werden in-place addiert
 */
static inline void hydra_neon_accumulate_chunk(
    const uint8_t *packed,
    const int8_t *state,
    int32_t token_val,
    int32_t *acc)
{
    uint8x16_t c = vld1q_u8(packed);

    /* Felder extrahieren: w1 = Bits 0-1, w2 = Bits 2-3 */
    uint8x16_t c1 = vandq_u8(c, vdupq_n_u8(3));
    uint8x16_t c2 = vandq_u8(vshrq_n_u8(c, 2), vdupq_n_u8(3));

    /* Decode: 01 -> +1, 10 -> -1, 00/11 -> 0 (identisch zur Skalar-Logik) */
    uint8x16_t p1 = vceqq_u8(c1, vdupq_n_u8(1));
    uint8x16_t n1 = vceqq_u8(c1, vdupq_n_u8(2));
    uint8x16_t p2 = vceqq_u8(c2, vdupq_n_u8(1));
    uint8x16_t n2 = vceqq_u8(c2, vdupq_n_u8(2));

    int8x16_t w1 = vsubq_s8(vreinterpretq_s8_u8(p1), vreinterpretq_s8_u8(n1));
    int8x16_t w2 = vsubq_s8(vreinterpretq_s8_u8(p2), vreinterpretq_s8_u8(n2));

    int8x16_t s = vld1q_s8(state);

    /* Auf 16-bit erweitern und Produkte bilden.
     * Overflow-Analyse: |w| <= 1, |token_val| <= 127, |state| <= 127
     * -> |Produkt| <= 16129, passt in int16. */
    int16x8_t w1_lo = vmovl_s8(vget_low_s8(w1));
    int16x8_t w1_hi = vmovl_s8(vget_high_s8(w1));
    int16x8_t w2_lo = vmovl_s8(vget_low_s8(w2));
    int16x8_t w2_hi = vmovl_s8(vget_high_s8(w2));
    int16x8_t s_lo  = vmovl_s8(vget_low_s8(s));
    int16x8_t s_hi  = vmovl_s8(vget_high_s8(s));

    int16x8_t tok16 = vdupq_n_s16((int16_t)token_val);

    int16x8_t sum_lo = vaddq_s16(vmulq_s16(w1_lo, tok16), vmulq_s16(w2_lo, s_lo));
    int16x8_t sum_hi = vaddq_s16(vmulq_s16(w1_hi, tok16), vmulq_s16(w2_hi, s_hi));

    /* Auf 32-bit erweitern und in die Akkumulatoren addieren */
    vst1q_s32(acc + 0, vaddq_s32(vld1q_s32(acc + 0), vmovl_s16(vget_low_s16(sum_lo))));
    vst1q_s32(acc + 4, vaddq_s32(vld1q_s32(acc + 4), vmovl_s16(vget_high_s16(sum_lo))));
    vst1q_s32(acc + 8, vaddq_s32(vld1q_s32(acc + 8), vmovl_s16(vget_low_s16(sum_hi))));
    vst1q_s32(acc + 12, vaddq_s32(vld1q_s32(acc + 12), vmovl_s16(vget_high_s16(sum_hi))));
}

#endif /* __ARM_NEON */
#endif /* HYDRA_NEON_H */
