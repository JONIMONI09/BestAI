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

/* Bit-pattern redefinition of uint8x16_t as int8x16_t. Recent
 * AArch64 arm_neon.h headers (clang 14+) only provide the
 * source-suffixed spelling vreinterpretq_s8_u8, while GCC's
 * arm_neon.h still only provides the plain vreinterpretq_s8. CI
 * compiles this header with BOTH (aarch64-linux-gnu-gcc-12 and
 * Apple clang on macos-latest), so the spelling is selected by
 * compiler instead of hardcoding one of them. */
#if defined(__clang__)
#define hydra_neon_u8x16_as_s8(v) vreinterpretq_s8_u8(v)
#else
#define hydra_neon_u8x16_as_s8(v) vreinterpretq_s8(v)
#endif

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

/**
 * Load-time kernel for a version-2 plane (4 ternary weights per byte,
 * weight j at bits 2j..2j+1 of byte j/4), NEON counterpart of
 * hydra_avx2_unpack_v2_plane in src/hydra_simd.h.
 *
 * The 2-bit -> signed decode is fully in-register: per 16-byte chunk
 * (= 64 dimensions) the four weight positions (2 bits each) are
 * right-shifted into the low 2 bits, masked with 0x03, and looked up in
 * a 16-byte table T[i] = ternary(i & 3) with vqtbl1q_u8 (the table's
 * low-4-bit index identity — the same table the scalar and AVX2
 * decoders use; code table 00=0, 01=+1, 10=-1, 11=0, -1 stored as 0xFF
 * and sign-extended on the widening).
 *
 * Layout: the decoded value for byte k, nibble j belongs to dimension
 * 4k + j. After widening, plane j gives four registers R_j (r = 0..3,
 * lane n) with R_j[r][n] = byte 4r+n, nibble j. Output block m covers
 * dimensions 16m..16m+15 (bytes 4m..4m+3) and needs, per byte, the
 * four nibbles in order: dim 16m+4b+c = R_c[m][b]. A 4-way interleave
 * is two rounds of 2-way vzipq_s32 with the pairs two apart (zip
 * alternates lanes, so pairing 0/2 with 1/3 and re-zipping groups):
 *   q02 = zip(R0_m, R2_m), q13 = zip(R1_m, R3_m)
 *   o01 = zip(q02[0], q13[0]) -> dims of bytes 4m, 4m+1
 *   o23 = zip(q02[1], q13[1]) -> dims of bytes 4m+2, 4m+3
 *
 * BIT-IDENTICAL to the scalar hydra_build_aggregation_v1_1: for every
 * dimension exactly ONE integer (0/+1/-1) from the same table is added
 * to the same int32 accumulator; integer addition of the same values
 * is independent of instruction shape. tests/test_engine.c proves it
 * (test_neon_v2_unpack, test_neon_v2_load_matches_scalar).
 *
 * Contract: dim % 64 == 0 (the engine's dim is 64). Processes the
 * whole plane: dim/4 bytes.
 */
static inline void hydra_neon_unpack_v2_plane(
    int32_t *agg,
    const uint8_t *plane,
    size_t dim)
{
    /* T[i] = ternary value of code (i & 3) as unsigned byte; 0xFF for
     * -1, sign-extended on the s8 -> s16 -> s32 widening below. 16
     * entries so any 4-bit index of the masked weights selects the
     * right low-2-bit class. aligned(16): vld1q_u8 faults on
     * misaligned addresses and a 16-byte static array is only
     * guaranteed 1-byte aligned by the C standard. */
    static const uint8_t T[16] __attribute__((aligned(16))) = {
        0x00, 0x01, 0xFF, 0x00, 0x00, 0x01, 0xFF, 0x00,
        0x00, 0x01, 0xFF, 0x00, 0x00, 0x01, 0xFF, 0x00
    };
    const uint8x16_t tv = vld1q_u8(T);

    for (size_t d = 0; d < dim; d += 64) {
        const uint8x16_t bytes = vld1q_u8(plane + d / 4);

        /* Normalize each weight to the low 2 bits (logical right
         * shifts), then mask with 0x03: vqtbl1q_u8 indexes with each
         * lane's LOW 4 bits only, so the weight must sit in bits 0-1
         * of the index (a pure AND mask like 0x3C/0xF0/0xC0 would
         * leave the weight in bits 2-7 and decode planes 1-3 as 0).
         *
         * vshrq_n_u8, not negative vshlq_n_u8 immediates: for
         * uint8 lanes a negative shift has the same bit pattern as a
         * positive logical right shift, but the negative form is not
         * accepted by all arm_neon.h implementations (their fixed
         * static-immediate macro/builtin rejects values outside the
         * positive encode range) — proven by aarch64 clang/GCC
         * builds. vshrq_n_u8 with 2/4/6 is in the legal 0..31 range
         * of the AArch64 LSR instruction on every implementation. */
        const uint8x16_t m3 = vdupq_n_u8(0x03);
        const uint8x16_t d0 = vqtbl1q_u8(tv, vandq_u8(bytes, m3));
        const uint8x16_t d1 = vqtbl1q_u8(tv, vandq_u8(vshrq_n_u8(bytes, 2), m3));
        const uint8x16_t d2 = vqtbl1q_u8(tv, vandq_u8(vshrq_n_u8(bytes, 4), m3));
        const uint8x16_t d3 = vqtbl1q_u8(tv, vandq_u8(vshrq_n_u8(bytes, 6), m3));

        /* Widen each decoded plane to 16 int32 (four int32x4 lanes):
         * Rj_r lane n = decoded weight for byte 4r+n, nibble j. */
        const int8x16_t s0 = hydra_neon_u8x16_as_s8(d0);
        const int8x16_t s1 = hydra_neon_u8x16_as_s8(d1);
        const int8x16_t s2 = hydra_neon_u8x16_as_s8(d2);
        const int8x16_t s3 = hydra_neon_u8x16_as_s8(d3);
        const int16x8_t s0a = vmovl_s8(vget_low_s8(s0));
        const int16x8_t s0b = vmovl_s8(vget_high_s8(s0));
        const int16x8_t s1a = vmovl_s8(vget_low_s8(s1));
        const int16x8_t s1b = vmovl_s8(vget_high_s8(s1));
        const int16x8_t s2a = vmovl_s8(vget_low_s8(s2));
        const int16x8_t s2b = vmovl_s8(vget_high_s8(s2));
        const int16x8_t s3a = vmovl_s8(vget_low_s8(s3));
        const int16x8_t s3b = vmovl_s8(vget_high_s8(s3));

        for (int m = 0; m < 4; ++m) {
            const int32x4_t r0 = m == 0 ? vmovl_s16(vget_low_s16(s0a))
                             : m == 1 ? vmovl_s16(vget_high_s16(s0a))
                             : m == 2 ? vmovl_s16(vget_low_s16(s0b))
                                      : vmovl_s16(vget_high_s16(s0b));
            const int32x4_t r1 = m == 0 ? vmovl_s16(vget_low_s16(s1a))
                             : m == 1 ? vmovl_s16(vget_high_s16(s1a))
                             : m == 2 ? vmovl_s16(vget_low_s16(s1b))
                                      : vmovl_s16(vget_high_s16(s1b));
            const int32x4_t r2 = m == 0 ? vmovl_s16(vget_low_s16(s2a))
                             : m == 1 ? vmovl_s16(vget_high_s16(s2a))
                             : m == 2 ? vmovl_s16(vget_low_s16(s2b))
                                      : vmovl_s16(vget_high_s16(s2b));
            const int32x4_t r3 = m == 0 ? vmovl_s16(vget_low_s16(s3a))
                             : m == 1 ? vmovl_s16(vget_high_s16(s3a))
                             : m == 2 ? vmovl_s16(vget_low_s16(s3b))
                                      : vmovl_s16(vget_high_s16(s3b));

            /* 4-way interleave in two rounds of 2-way vzipq. The first
             * round pairs the planes two apart: vzipq alternates lanes,
             * so zipping (0,2) and (1,3) and then re-zipping GROUPS the
             * nibbles in dimension order instead of alternating them.
             *   o01.val[0] = (r0[0],r1[0],r2[0],r3[0]) = dims of byte 4m
             *   o01.val[1] = (r0[1],r1[1],r2[1],r3[1]) = dims of byte 4m+1
             *   o23.val[0] = (r0[2],r1[2],r2[2],r3[2]) = dims of byte 4m+2
             *   o23.val[1] = (r0[3],r1[3],r2[3],r3[3]) = dims of byte 4m+3
             */
            const int32x4x2_t q02 = vzipq_s32(r0, r2);
            const int32x4x2_t q13 = vzipq_s32(r1, r3);
            const int32x4x2_t o01 = vzipq_s32(q02.val[0], q13.val[0]);
            const int32x4x2_t o23 = vzipq_s32(q02.val[1], q13.val[1]);

            const size_t base = d + (size_t)m * 16;
            vst1q_s32(agg + base + 0,
                      vaddq_s32(vld1q_s32(agg + base + 0), o01.val[0]));
            vst1q_s32(agg + base + 4,
                      vaddq_s32(vld1q_s32(agg + base + 4), o01.val[1]));
            vst1q_s32(agg + base + 8,
                      vaddq_s32(vld1q_s32(agg + base + 8), o23.val[0]));
            vst1q_s32(agg + base + 12,
                      vaddq_s32(vld1q_s32(agg + base + 12), o23.val[1]));
        }
    }
}

#endif /* __ARM_NEON */
#endif /* HYDRA_NEON_H */