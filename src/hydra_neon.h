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
#include <string.h>

/* The in-register version-2 plane unpack below uses the 128-bit
 * table-lookup intrinsic vqtbl1q_u8, which exists only in the AArch64
 * (64-bit) arm_neon.h headers; the 32-bit ARM headers provide
 * vtbl1_u8 with a different lane shape instead. The Android NDK builds
 * this header for armeabi-v7a as well, so the kernel is compiled for
 * AArch64 only. On 32-bit ARM the engine takes the scalar v2 decode at
 * load time (hydra_build_aggregation) — bit-identical result, the
 * difference is one model load instead of every token. */
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define HYDRA_NEON_V2_UNPACK 1
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

#if defined(HYDRA_NEON_V2_UNPACK)

/* Bit-pattern redefinition of uint8x16_t as int8x16_t.
 *
 * The AArch64 arm_neon.h headers of BOTH CI toolchains (GCC's
 * gcc-12-aarch64-linux-gnu and Apple clang on macos-latest) use the
 * source-suffixed spelling vreinterpretq_s8_u8; the unsuffixed
 * vreinterpretq_s8 belongs to the 32-bit ARM headers and is not
 * declared for AArch64 (proven by the CI compile of gcc-12:
 * "implicit declaration of function 'vreinterpretq_s8'; did you
 * mean 'vreinterpretq_u8_s8'?"). The suffixed name is the one
 * spelling every AArch64 toolchain declares. */
#define hydra_neon_u8x16_as_s8(v) vreinterpretq_s8_u8(v)

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

#endif /* HYDRA_NEON_V2_UNPACK */

/* Transformer-format (v3/v4) core: the 2-bit ternary row dot
 *     sum_b T3[code(b)] * x[b],  code(b) = row byte b/4, bits 2*(b%4)
 * that every per-layer projection (Wg/Wu in v3, Wg/Wu/Wq/Wk/Wv plus
 * W_out in v4) is made of. NEON counterpart of hydra_avx2_tf_row_dot
 * (src/hydra_simd.h) - same chunking: one row byte carries FOUR 2-bit
 * codes, so 8 dims take 2 row bytes + 8 state bytes; same scalar tail
 * for dim % 8 == 4.
 *
 * INTEGER EXACT, bit-for-bit the scalar loop: the 2-bit codes come out
 * of the same bit positions the scalar loop reads (nibble & 3 / >> 2
 * & 3 - pure ALU, no lookup table at all, which is what also keeps
 * this on ARMv7-A where the 8-byte vtbl1_u8 cannot hold a 16-entry
 * table), vsub/vadd assemble T3 = {0, 1, -1, 0} from equality masks,
 * vmovl_s8 keeps every value in its native range, and vmlal_s16
 * (signed x signed -> int32) plus the 4-lane sum cannot overflow
 * (worst case dim 64: 64 * 127 = 8128). No float reassociation, so the
 * parity suite (tests/test_engine.c) can memcmp against the scalar
 * reference and the pinned v3/v4 streams stay valid.
 *
 * dim % 4 == 0 is the loader's contract for v3/v4 (tf_layer_bytes), so
 * row has exactly dim/4 bytes (<= 16). dim runs from 4 to
 * HYDRA_EMBED_DIM. */
static inline int32_t hydra_neon_tf_row_dot(const uint8_t *row,
                                            const int8_t *x,
                                            uint32_t dim)
{
    const uint8x8_t m3 = vdup_n_u8(0x03);
    int32x4_t acc = vdupq_n_s32(0);

    /* Zero-filled row copy. The per-chunk row read is 8 bytes wide
     * (one 64-bit register) even though a chunk owns 2 row bytes, so
     * the buffer must hold dim/4 + 8 (worst case dim 64: 16 + 8 = 24)
     * - only lanes 0/1 of the read are used, and the padding is 0
     * (code 0 / weight 0). */
    uint8_t rp[24];
    memset(rp, 0, sizeof(rp));
    memcpy(rp, row, dim / 4);

    uint32_t b = 0;
    for (; b + 8 <= dim; b += 8) {
        /* 2 row bytes = 8 codes; 8 state bytes = 8 x's. The row byte
         * for dim d is row[d/4] (4 dims per byte), so the chunk's two
         * row bytes sit at b/4 - the load is 8 bytes wide (one 64-bit
         * NEON register), but only lanes 0/1 are ever read below. */
        uint8_t rb[8];
        memcpy(rb, rp + (b / 4), 8);
        uint8x8_t V = vld1_u8(rb);
        int8x8_t xv = vld1_s8(x + b);
        /* The two nibbles of the two row bytes. vshr_n_u8 is the
         * portable per-byte shift (the vshl_n_u8(x, -n) spelling is
         * a GNU extension and not accepted by every toolchain's
         * arm_neon.h). */
        uint8x8_t n0 = vand_u8(V, vdup_n_u8(0x0F));
        uint8x8_t n1 = vshr_n_u8(V, 4);
        /* Codes of the 8 dims, two at a time:
         *   E0 = [c0, c4, ...], E1 = [c1, c5, ...]
         *   C0 = [c2, c6, ...], C1 = [c3, c7, ...]
         * (padding bytes hold nibble 0 -> code 0.) */
        uint8x8_t E0 = vand_u8(n0, m3);
        uint8x8_t C0 = vand_u8(n1, m3);
        uint8x8_t E1 = vand_u8(vshr_n_u8(n0, 2), m3);
        uint8x8_t C1 = vand_u8(vshr_n_u8(n1, 2), m3);
        /* w = [c0, c1, c2, c3, c4, c5, c6, c7] from the four
         * [c?, c?, 0, ...] vectors: low 2 bytes of each, in order
         * (vld1_s8 from a byte array - the portable way to build an
         * 8-lane vector from scalars). */
        const int8_t warr[8] = {
            (int8_t)vget_lane_u8(E0, 0), (int8_t)vget_lane_u8(E1, 0),
            (int8_t)vget_lane_u8(C0, 0), (int8_t)vget_lane_u8(C1, 0),
            (int8_t)vget_lane_u8(E0, 1), (int8_t)vget_lane_u8(E1, 1),
            (int8_t)vget_lane_u8(C0, 1), (int8_t)vget_lane_u8(C1, 1)
        };
        int8x8_t Wc = vld1_s8(warr);
        /* T3 = {0, 1, -1, 0} from equality masks: +1 where code 1
         * (the 0xFF mask is -1 as int8, negated), -1 where code 2.
         * vceq_u8 (not vceqq_u8) is the 64-bit form - the q-suffixed
         * intrinsic takes 128-bit vectors on every toolchain. */
        const int8x8_t pos =
            vreinterpret_s8_u8(vceq_u8(vreinterpret_u8_s8(Wc), vdup_n_u8(1)));
        const int8x8_t neg =
            vreinterpret_s8_u8(vceq_u8(vreinterpret_u8_s8(Wc), vdup_n_u8(2)));
        int8x8_t w = vsub_s8(vneg_s8(pos), vand_s8(neg, vdup_n_s8(1)));
        /* Widen signed and two-products-per-lane, both 4-lane halves:
         * [w0*x0 + w1*x1, w2*x2 + w3*x3, w4*x4 + w5*x5, w6*x6 + w7*x7]. */
        int16x8_t w16 = vmovl_s8(w);
        int16x8_t x16 = vmovl_s8(xv);
        acc = vmlal_s16(acc, vget_low_s16(w16), vget_low_s16(x16));
        acc = vmlal_s16(acc, vget_high_s16(w16), vget_high_s16(x16));
    }
    for (; b < dim; b += 4) {
        /* The tail (dim % 8 == 4, i.e. dims 4, 12, 20, ...) is a single
         * 4-dim unit: decode it the scalar way into a lane. */
        const uint32_t by = row[b / 4];
        const int8_t T3[4] = {0, 1, -1, 0};
        int32_t t = 0;
        t += (int32_t)T3[by & 0x03u] * (int32_t)x[b];
        t += (int32_t)T3[(by >> 2u) & 0x03u] * (int32_t)x[b + 1];
        t += (int32_t)T3[(by >> 4u) & 0x03u] * (int32_t)x[b + 2];
        t += (int32_t)T3[(by >> 6u) & 0x03u] * (int32_t)x[b + 3];
        acc = vaddq_s32(acc, vsetq_lane_s32(t, vdupq_n_s32(0), 0));
    }

    return vgetq_lane_s32(acc, 0) + vgetq_lane_s32(acc, 1)
         + vgetq_lane_s32(acc, 2) + vgetq_lane_s32(acc, 3);
}

#endif /* __ARM_NEON */
#endif /* HYDRA_NEON_H */