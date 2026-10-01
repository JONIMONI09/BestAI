/* Hydra NEON Ternary Accumulation Kernel (ARMv7-A / AArch64)
 *
 * 16 ternary weight/activation Paare werden parallel via NEON verarbeitet.
 * Nur Addition/Subtraktion — keine FP-Multiplikation.
 *
 * Kompiliert nur auf ARM; wird via Makefile-Check (__ARM_NEON) eingebunden.
 */
#ifndef HYDRA_NEON_H
#define HYDRA_NEON_H

#ifdef __ARM_NEON

#include <arm_neon.h>
#include <stdint.h>

/**
 * Akkumuliert 16 ternary products via NEON SIMD.
 * @param act       16 int8 Aktivierungen
 * @param codes     16 2-bit ternary codes (ein Byte pro Pair, untere 2 Bits relevant)
 * @return Summe aller w_i * act_i als int32
 */
static inline int32_t hydra_neon_accumulate_16(
    const int8_t *act,
    const uint8_t *codes)
{
    int8x16_t  x = vld1q_s8(act);
    uint8x16_t c = vld1q_u8(codes);

    /* Decode: code==1 -> +1, code==2 -> -1, sonst 0 */
    uint8x16_t is_pos = vceqq_u8(c, vdupq_n_u8(1));
    uint8x16_t is_neg = vceqq_u8(c, vdupq_n_u8(2));

    int8x16_t w = vsubq_s8(vreinterpretq_s8_u8(is_pos),
                           vreinterpretq_s8_u8(is_neg));

    /* 8-bit Produkt via Expansion auf 16-bit */
    int16x8_t x_lo = vmovl_s8(vget_low_s8(x));
    int16x8_t x_hi = vmovl_s8(vget_high_s8(x));
    int16x8_t w_lo = vmovl_s8(vget_low_s8(w));
    int16x8_t w_hi = vmovl_s8(vget_high_s8(w));

    int16x8_t prod_lo = vmulq_s16(x_lo, w_lo);
    int16x8_t prod_hi = vmulq_s16(x_hi, w_hi);

    /* Paarweise Addition -> 32-bit */
    int32x4_t acc_lo = vpaddlq_s16(prod_lo);
    int32x4_t acc_hi = vpaddlq_s16(prod_hi);
    int32x4_t total  = vaddq_s32(acc_lo, acc_hi);

    int32x2_t r = vpadd_s32(vget_low_s32(total), vget_high_s32(total));
    r = vpadd_s32(r, r);
    return vget_lane_s32(r, 0);
}

#endif /* __ARM_NEON */
#endif /* HYDRA_NEON_H */
