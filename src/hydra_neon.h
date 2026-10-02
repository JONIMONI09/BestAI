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
 * BIT-IDENTISCH-GARANTIE: der Dekodier-Schritt bildet pro Code ein
 * Maskenbyte 0x01 (+1) bzw. 0xFF (-1) und interpretiert es als int8_t.
 * vceqq_u8 liefert 0xFF/0x00; ein frueherer vsubq-Trick
 * (vsubq_s8(p, n)) rechnete daher 0xFF-0x00 = -1 fuer den Code 01
 * und INVERTIERTE das Vorzeichen aller Gewichte. Der untenstehende
 * Masken-Ansatz ist korrekt und zusaetzlich robust gegen eine
 * Aenderung der Bitbreite. tests/test_engine.c vergleicht diesen Pfad
 * direkt gegen die skalare Referenz (test_neon_matches_scalar).
 */
#ifndef HYDRA_NEON_H
#define HYDRA_NEON_H

#ifdef __ARM_NEON
#define HYDRA_USE_NEON 1
#include <arm_neon.h>
#include <stdint.h>

/* Nimmt 4 int32-Lanes und addiert sie auf 4 int64-Akkumulatoren
 * (acc[0..3]). vaddq_s64 arbeitet auf je 2 Lanes. */
static inline void hydra_neon_acc4(int64_t *acc, int32x4_t v)
{
    vst1q_s64(acc + 0, vaddq_s64(vld1q_s64(acc + 0), vmovl_s32(vget_low_s32(v))));
    vst1q_s64(acc + 2, vaddq_s64(vld1q_s64(acc + 2), vmovl_s32(vget_high_s32(v))));
}

/**
 * Akkumuliert einen 16-Lane-Chunk ternarer Produkte:
 *   acc[i] += w1(packed[i]) * token + w2(packed[i]) * state[i]
 *
 * @param packed    16 Bytes im Format- Packing (2 Gewichte/Byte)
 * @param state     16 int8 State-Werte (Layer-Eingang)
 * @param token_val Token-Wert (0..HYDRA_MAX_VOCAB-1, siehe step())
 * @param acc       16 int64-Akkumulatoren, werden in-place addiert
 *
 * Overflow-Analyse: |w| <= 1, |token| <= 1023, |state| <= 127
 * -> |Produkt| <= 1150, passt in int16. Die Addition in den
 * int64-Akkumulator ist damit ueber alle Layer hinweg overflow-frei.
 */
static inline void hydra_neon_accumulate_chunk(
    const uint8_t *packed,
    const int8_t *state,
    int32_t token_val,
    int64_t *acc)
{
    uint8x16_t c = vld1q_u8(packed);

    /* Felder extrahieren: w1 = Bits 0-1, w2 = Bits 2-3 */
    uint8x16_t c1 = vandq_u8(c, vdupq_n_u8(3));
    uint8x16_t c2 = vandq_u8(vshrq_n_u8(c, 2), vdupq_n_u8(3));

    /* Decode: 01 -> +1, 10 -> -1, 00/11 -> 0 (identisch zur Skalar-Logik).
     * p/n sind Vergleichsmasken (0xFF = Treffer); erst auf 0x01
     * normalisieren, dann interpretieren wir 0x01 als +1 und 0xFF als -1.
     * vreinterpretq_s8_u8(0x01) = +1, vreinterpretq_s8_u8(0xFF) = -1. */
    const uint8x16_t one = vdupq_n_u8(1);
    const uint8x16_t p1 = vandq_u8(vceqq_u8(c1, vdupq_n_u8(1)), one);
    const uint8x16_t n1 = vandq_u8(vceqq_u8(c1, vdupq_n_u8(2)), one);
    const uint8x16_t p2 = vandq_u8(vceqq_u8(c2, vdupq_n_u8(1)), one);
    const uint8x16_t n2 = vandq_u8(vceqq_u8(c2, vdupq_n_u8(2)), one);

    /* 0x01 (pos) - 0x01 (neg) = 0x00, 0x00 - 0x01 = 0xFF -> -1 */
    const int8x16_t w1 = vreinterpretq_s8_u8(vsubq_u8(p1, n1));
    const int8x16_t w2 = vreinterpretq_s8_u8(vsubq_u8(p2, n2));

    const int8x16_t s = vld1q_s8(state);

    /* Auf 16-bit erweitern und Produkte bilden. */
    int16x8_t w1_lo = vmovl_s8(vget_low_s8(w1));
    int16x8_t w1_hi = vmovl_s8(vget_high_s8(w1));
    int16x8_t w2_lo = vmovl_s8(vget_low_s8(w2));
    int16x8_t w2_hi = vmovl_s8(vget_high_s8(w2));
    int16x8_t s_lo  = vmovl_s8(vget_low_s8(s));
    int16x8_t s_hi  = vmovl_s8(vget_high_s8(s));

    int16x8_t tok16 = vdupq_n_s16((int16_t)token_val);

    int16x8_t sum_lo = vaddq_s16(vmulq_s16(w1_lo, tok16), vmulq_s16(w2_lo, s_lo));
    int16x8_t sum_hi = vaddq_s16(vmulq_s16(w1_hi, tok16), vmulq_s16(w2_hi, s_hi));

    /* int16 -> int32 -> int64 und in die Akkumulatoren addieren.
     * sum_lo (Lanes 0-7) und sum_hi (Lanes 8-15) ergeben zusammen 16
     * Lanes; acc4() verarbeitet je 4, die Offsets sind daher 0/4/8/12. */
    hydra_neon_acc4(acc + 0,  vmovl_s16(vget_low_s16(sum_lo)));
    hydra_neon_acc4(acc + 4,  vmovl_s16(vget_high_s16(sum_lo)));
    hydra_neon_acc4(acc + 8,  vmovl_s16(vget_low_s16(sum_hi)));
    hydra_neon_acc4(acc + 12, vmovl_s16(vget_high_s16(sum_hi)));
}

#endif /* __ARM_NEON */
#endif /* HYDRA_NEON_H */