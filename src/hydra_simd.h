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

#endif /* __AVX2__ */

#endif /* HYDRA_SIMD_H */
