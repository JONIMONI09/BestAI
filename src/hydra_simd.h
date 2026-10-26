#ifndef HYDRA_SIMD_H
#define HYDRA_SIMD_H

/* ---------------------------------------------------------------------
 * x86-64 AVX2 kernels for the engine (M3).
 *
 * Register-width reminder (the source of the first revision's bugs):
 * a __m256i holds 8 int32, not 16, and a __m128i holds 4 int32.
 *
 * Exactness of the step kernel (why int32 lanes are legal):
 *   |A[i]|, |B[i]| <= HYDRA_MAX_LAYERS = 4096,
 *   |token_val| <= 1023 (vocab cap), |state[i]| <= 127,
 *   |A[i]*t + B[i]*s| <= 4096*1023 + 4096*127 = 4,710,400 < INT32_MAX.
 * The products and their sum therefore cannot wrap in 32 bits, and the
 * int32 -> int64 widening below is a pure re-interpretation. The result
 * is BIT-IDENTICAL to the scalar expression
 *   (int64)A[i]*t + (int64)B[i]*(int32)s[i]
 * because both evaluate the same integer operations; the AVX2 parity
 * test in tests/test_engine.c is the proof.
 *
 * Build/detect contract:
 *   - The Makefile adds -mavx2 only on x86-64 hosts where the compiler
 *     accepts it; that also defines __AVX2__ for the preprocessor.
 *   - hydra_simd_has_avx2() is a RUNTIME check (CPUID via
 *     __builtin_cpu_supports), so a -mavx2 build still runs — on the
 *     scalar path — on CPUs without AVX2, and a plain build never
 *     contains AVX2 instructions at all (no SIGILL risk anywhere).
 *
 * Scope note (honest): the STEP kernel (per-token hot path) is the AVX2
 * win. The version-2 load-time unpack is accelerated only in the cheap
 * 2-bit -> signed decoding (the LUT); the 4-weights-per-byte scatter to
 * stride-4 dimension slots is a small scalar loop, because a 4x16 byte
 * transpose is not cleanly expressible with plain AVX2 shuffles and the
 * operation runs once at load time, not per token.
 * ------------------------------------------------------------------- */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

/* 1 = the running CPU supports AVX2, 0 otherwise. Always available;
 * in non-AVX2 builds it is a constant zero. */
int hydra_simd_has_avx2(void);

#if defined(__AVX2__)

/* Step-time kernel: writes accumulator[i..i+8) = A[i..]*t + B[i..]*s.
 * 8 dimensions per iteration (one 256-bit A/B lane each, 8 int8 state
 * widened to 8 int32). The int32 intermediate sum is exact (see the
 * exactness proof above) and is widened to int64 per 4-dim half.
 * Each accumulator slot is written exactly once per step (the caller's
 * accumulator is zero-initialised), so a plain store — not an add — is
 * correct. Callers must pass a region of 8 dims. */
static void hydra_avx2_accumulate_agg(int64_t *acc,
                                      const int32_t *a,
                                      const int32_t *b,
                                      const int8_t *state,
                                      int32_t token_val)
{
    const __m256i t8 = _mm256_set1_epi32(token_val);
    const __m256i a8 = _mm256_loadu_si256((const __m256i *)a);   /* 8 int32 */
    const __m256i b8 = _mm256_loadu_si256((const __m256i *)b);   /* 8 int32 */

    /* 8 int8 -> 8 int32: two 4-wide widenings (low/high byte quad),
     * combined into one 256-bit lane. _mm_loadu_si64 reads exactly the
     * 8 state bytes (no over-read past the 8-dim chunk). */
    const __m128i s_in = _mm_loadu_si64(state);
    const __m128i s_lo = _mm_cvtepi8_epi32(s_in);                 /* dims 0-3 */
    const __m128i s_hi = _mm_cvtepi8_epi32(_mm_srli_si128(s_in, 4)); /* dims 4-7 */
    const __m256i s8 = _mm256_inserti128_si256(_mm256_castsi128_si256(s_lo), s_hi, 1);

    const __m256i sum32 = _mm256_add_epi32(_mm256_mullo_epi32(a8, t8),
                                           _mm256_mullo_epi32(b8, s8));

    const __m128i lo32 = _mm256_castsi256_si128(sum32);          /* dims 0-3 */
    const __m128i hi32 = _mm256_extracti128_si256(sum32, 1);     /* dims 4-7 */
    _mm256_storeu_si256((__m256i *)(acc),     _mm256_cvtepi32_epi64(lo32));
    _mm256_storeu_si256((__m256i *)(acc + 4), _mm256_cvtepi32_epi64(hi32));
}

/* Load-time kernel for a version-2 plane (4 ternary weights per byte,
 * weight j at bits 2j..2j+1 of byte j/4). The 2-bit -> signed decoding
 * is the LUT (one 128-bit shuffle per weight position); the result is
 * scattered to stride-4 dimension slots with a small scalar loop
 * (see the scope note above). Processes 16 bytes = 64 dimensions per
 * iteration; the caller must pass dim % 64 == 0 (i.e. dim == 64, the
 * engine's embedded dimension). */
static void hydra_avx2_unpack_v2_plane(int32_t *agg, const uint8_t *plane,
                                       size_t dim)
{
    /* T[i] = ternary value of code (i & 3), as int8; -1 stored as 0xFF.
     * The 128-bit shuffle indexes with (byte & 0x0F), so 16 entries
     * suffice and entry i maps to code i & 3 (no masking needed). */
    static uint8_t T[16];
    static int ready = 0;
    if (!ready) {
        static const int8_t map[4] = {0, 1, -1, 0};
        for (int i = 0; i < 16; ++i) T[i] = (uint8_t)map[i & 3];
        ready = 1;
    }
    const __m128i table = _mm_loadu_si128((const __m128i *)T);

    /* Per-byte extraction with leak-proof 16-bit lanes: the mask keeps
     * the source bits AND zeroes the previous byte's bits that the
     * 16-bit shift would otherwise leak in.
     *   w0: bits 0-1  (no shift)
     *   w1: bits 2-3  (>> 2, mask 0x3F)
     *   w2: bits 4-5  (>> 4, mask 0xF0)
     *   w3: bits 6-7  (>> 6, mask 0xC0) */
    const __m128i mask_w1 = _mm_set1_epi8((int8_t)0x3F);
    const __m128i mask_w2 = _mm_set1_epi8((int8_t)0xF0);
    const __m128i mask_w3 = _mm_set1_epi8((int8_t)0xC0);

    for (size_t d = 0; d < dim; d += 64) {
        const __m128i bytes = _mm_loadu_si128((const __m128i *)(plane + d / 4));
        /* Every shuffle INDEX must be masked to 4 bits: VPSHUFB zeroes
         * any lane whose index has bit 7 set. The shifted values can
         * carry leaked bits into bits 2-7 (the 16-bit lane shift pulls
         * the next byte's low bits in), so a per-byte & 0x0F on the
         * index is mandatory; the code only ever lives in the low 2
         * bits and the table absorbs bits 2-3 via i & 3. */
        const __m128i mask_idx = _mm_set1_epi8((int8_t)0x0F);
        const __m128i w0 = _mm_shuffle_epi8(table, _mm_and_si128(bytes, mask_idx));
        const __m128i w1 = _mm_shuffle_epi8(table,
            _mm_and_si128(_mm_srli_epi16(_mm_and_si128(bytes, mask_w1), 2), mask_idx));
        const __m128i w2 = _mm_shuffle_epi8(table,
            _mm_and_si128(_mm_srli_epi16(_mm_and_si128(bytes, mask_w2), 4), mask_idx));
        const __m128i w3 = _mm_shuffle_epi8(table,
            _mm_and_si128(_mm_srli_epi16(_mm_and_si128(bytes, mask_w3), 6), mask_idx));

        int8_t v0[16], v1[16], v2[16], v3[16];
        _mm_storeu_si128((__m128i *)v0, w0);
        _mm_storeu_si128((__m128i *)v1, w1);
        _mm_storeu_si128((__m128i *)v2, w2);
        _mm_storeu_si128((__m128i *)v3, w3);
        for (int k = 0; k < 16; ++k) {
            agg[d + 4 * k + 0] += v0[k];
            agg[d + 4 * k + 1] += v1[k];
            agg[d + 4 * k + 2] += v2[k];
            agg[d + 4 * k + 3] += v3[k];
        }
    }
}

/* Transformer-format (v3/v4) core: the 2-bit ternary row dot
 *     sum_b T3[code(b)] * x[b],  code(b) = row byte b/4, bits 2*(b%4)
 * that every per-layer projection (Wg/Wu in v3, Wg/Wu/Wq/Wk/Wv plus
 * W_out in v4) is made of. One row byte carries FOUR 2-bit codes, so
 * 8 dims (8 codes) take 2 row bytes + 8 state bytes.
 *
 * INTEGER EXACT, bit-for-bit the scalar loop: the 2-bit codes come out
 * of the same bit positions the scalar loop reads (low nibble & 3 /
 * >> 2 & 3, high nibble the same), pshufb decodes them through the
 * SAME table (T3 = {0, 1, -1, 0}; the padded bytes of the zero-filled
 * row buffer hold code 0, which decodes to weight 0 - and the loop
 * bound is the dim itself, so no padding ever multiplies anything),
 * cvtepi8_epi16 keeps every value in its native range, and
 * madd_epi16 (signed x signed -> int32) plus the 4-lane sum cannot
 * overflow (worst case dim 64: 64 * 127 = 8128). No reassociation of
 * FLOAT values happens anywhere, which is what makes the parity suite
 * (tests/test_engine.c) able to memcmp the result against the scalar
 * reference - the v3/v4 pinned token streams stay valid on AVX2
 * machines without re-pinning.
 *
 * dim % 4 == 0 is the loader's contract for v3/v4 (tf_layer_bytes),
 * so row has exactly dim/4 bytes (<= 16). The tail handles dim % 8 == 4
 * (dims 4, 12, 20, ...): one scalar 4-dim unit. Callers may pass dim
 * from 4 to HYDRA_EMBED_DIM (64). */
static inline int32_t hydra_avx2_tf_row_dot(const uint8_t *row,
                                            const int8_t *x,
                                            uint32_t dim)
{
    /* T3 = {0, 1, -1, 0}; positions 4..15 are 0 - a corrupt caller
     * with code > 3 would read 0 here exactly as it would index past
     * the scalar table's 4 entries - same contract. Local, not static:
     * an intrinsic call is not a C constant expression and may not
     * initialise file-scope storage. */
    const __m128i t3 =
        _mm_setr_epi8(0, 1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    /* 2-bit code extraction tables (16-entry, pshufb's index space):
     * ta[n] = n & 3          -> codes 0/4 of a byte
     *      (nibble low bits)  (low nibble of byte 0, of byte 1)
     * tb[n] = (n >> 2) & 3   -> codes 1/5 of a byte */
    const __m128i ta = _mm_setr_epi8(
        0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3);
    const __m128i tb = _mm_setr_epi8(
        0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3);
    const __m128i f0f  = _mm_set1_epi8(0x0F);
    const __m128i f0f0 = _mm_set1_epi8(0xF0);
    /* Only bytes 0/1 of the 8-byte row load carry THIS chunk's codes.
     * The load is a full 8 bytes even though a chunk owns 2 row bytes,
     * so bytes 2..7 hold the NEXT chunk's row data whenever dim > 8.
     * pshufb and unpacklo touch all 16 lanes, so mask the nibble
     * vectors down to the chunk's own 2 bytes before any table
     * lookup - otherwise later dims' codes leak into this chunk's
     * interleave (V[4..5] reach p0[8..11], colliding with the q0
     * region after the shift+OR). */
    const __m128i chunk2 = _mm_setr_epi8(
        0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    __m128i acc = _mm_setzero_si128();

    /* Zero-filled row copy. The per-chunk row load is 8 bytes wide
     * even though a chunk owns 2 row bytes, so the buffer must hold
     * dim/4 + 8 bytes (worst case dim 64: 16 + 8 = 24); the padding
     * is 0 (code 0 / weight 0) and the chunk mask keeps it away from
     * the result, but the load must not run past the buffer. */
    uint8_t rp[24];
    memset(rp, 0, sizeof(rp));
    memcpy(rp, row, dim / 4);
    /* Same padding problem on the state side: the 16-byte x load only
     * uses 8 bytes, and the last chunk (b = dim-8) would run 8 bytes
     * past a caller buffer of exactly dim bytes (UB, caught by ASan).
     * dim + 8 <= 72 covers the load for every dim <= 64; the upper
     * half never multiplies anything (madd_epi16 uses lanes 0..7
     * only). The tail loop still reads the caller's x directly. */
    int8_t xp[72];
    memset(xp, 0, sizeof(xp));
    memcpy(xp, x, dim);

    uint32_t b = 0;
    for (; b + 8 <= dim; b += 8) {
        /* 2 row bytes = 8 codes; 8 state bytes = 8 x's. Both reads
         * stay inside the padded copies (rp, xp). Row byte for dim d
         * is row[d/4] (4 dims per byte), so the chunk starts at b/4
         * - NOT b/2 (that would skip a byte per chunk). */
        const __m128i V  = _mm_loadl_epi64((const __m128i *)(rp + (b / 4)));
        const __m128i xv = _mm_loadu_si128((const __m128i *)(xp + b));
        /* The two nibbles of the two row bytes. The high nibble:
         * mask to 0xF0, then a plain 16-bit-lane right shift by 4.
         * The mask already zeros each byte's low nibble, so
         * (b1<<8 | b0) >> 4 gives byte 0 = b0>>4 and byte 1 = b1>>4
         * with nothing crossing - no per-byte shift required. */
        const __m128i n0 =
            _mm_and_si128(_mm_and_si128(V, f0f), chunk2);
        const __m128i n1 =
            _mm_and_si128(_mm_srli_epi16(_mm_and_si128(V, f0f0), 4),
                          chunk2);
        /* Codes of the 8 dims, two at a time per table:
         *   E0 = [c0, c4, 0...], E1 = [c1, c5, 0...]
         *   C0 = [c2, c6, 0...], C1 = [c3, c7, 0...]
         * (padded positions read table entry 0 = code 0.) */
        const __m128i E0 = _mm_shuffle_epi8(ta, n0);
        const __m128i C0 = _mm_shuffle_epi8(ta, n1);
        const __m128i E1 = _mm_shuffle_epi8(tb, n0);
        const __m128i C1 = _mm_shuffle_epi8(tb, n1);
        /* w = [c0, c1, c2, c3, c4, c5, c6, c7]: pack the low bytes of
         * each vector (p0 = c0 c1 c4 c5, q0 = c2 c3 c6 c7), move q0
         * to the upper half, then ONE pshufb applies the fixed 2+2
         * permutation that no unpack sequence expresses. */
        const __m128i p0 = _mm_unpacklo_epi8(E0, E1);
        const __m128i q0 = _mm_slli_si128(_mm_unpacklo_epi8(C0, C1), 8);
        const __m128i T  = _mm_or_si128(p0, q0);
        const __m128i perm = _mm_setr_epi8(
            0, 1, 8, 9, 2, 3, 10, 11, 0, 0, 0, 0, 0, 0, 0, 0);
        const __m128i C  = _mm_shuffle_epi8(T, perm);
        /* Decode through the SAME table the scalar loop uses, widen
         * signed, and two-products-per-lane: 4 x int32 =
         * [w0*x0 + w1*x1, w2*x2 + w3*x3, w4*x4 + w5*x5, w6*x6 + w7*x7]. */
        const __m128i w   = _mm_shuffle_epi8(t3, C);
        const __m128i w16 = _mm_cvtepi8_epi16(w);
        const __m128i x16 = _mm_cvtepi8_epi16(xv);
        acc = _mm_add_epi32(acc, _mm_madd_epi16(w16, x16));
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
        acc = _mm_add_epi32(acc, _mm_cvtsi32_si128(t));
    }

    const __m128i h = _mm_hadd_epi32(acc, acc);
    return _mm_extract_epi32(h, 0) + _mm_extract_epi32(h, 1);
}

#endif /* __AVX2__ */

#endif /* HYDRA_SIMD_H */
