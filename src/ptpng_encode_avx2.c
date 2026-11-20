/* Forward PNG filters. This translation unit is built with AVX2 enabled. */
#include "ptpng_internal.h"

uint64_t ptpng_encode_score_avx2(const uint8_t *src, size_t count)
{
    __m256i sum0 = _mm256_setzero_si256(), sum1 = _mm256_setzero_si256();
    const __m256i zero = _mm256_setzero_si256();
    uint64_t lanes[4], score;
    size_t i = 0;
    for (; count - i >= 128; i += 128) {
        __m256i b0 = _mm256_loadu_si256((const __m256i *)(src + i));
        __m256i b1 = _mm256_loadu_si256((const __m256i *)(src + i + 32));
        __m256i b2 = _mm256_loadu_si256((const __m256i *)(src + i + 64));
        __m256i b3 = _mm256_loadu_si256((const __m256i *)(src + i + 96));
        /* abs(-128) has byte value 128, which SAD treats as unsigned. */
        sum0 = _mm256_add_epi64(sum0, _mm256_sad_epu8(_mm256_abs_epi8(b0), zero));
        sum1 = _mm256_add_epi64(sum1, _mm256_sad_epu8(_mm256_abs_epi8(b1), zero));
        sum0 = _mm256_add_epi64(sum0, _mm256_sad_epu8(_mm256_abs_epi8(b2), zero));
        sum1 = _mm256_add_epi64(sum1, _mm256_sad_epu8(_mm256_abs_epi8(b3), zero));
    }
    for (; count - i >= 32; i += 32) {
        __m256i bytes = _mm256_loadu_si256((const __m256i *)(src + i));
        sum0 = _mm256_add_epi64(sum0, _mm256_sad_epu8(_mm256_abs_epi8(bytes), zero));
    }
    _mm256_storeu_si256((__m256i *)lanes, _mm256_add_epi64(sum0, sum1));
    score = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < count; ++i) {
        unsigned x = src[i];
        score += x < 128 ? x : 256-x;
    }
    return score;
}

static void encode_tail_avx2(uint8_t *dst, const uint8_t *src,
                             const uint8_t *prev, size_t begin, size_t end,
                             unsigned bpp, int filter)
{
    size_t i;
    for (i = begin; i < end; ++i) {
        unsigned a = i >= bpp ? src[i - bpp] : 0;
        unsigned b = prev ? prev[i] : 0;
        unsigned predictor = 0;
        if (filter == 1) predictor = a;
        else if (filter == 2) predictor = b;
        else if (filter == 3) predictor = (a + b) >> 1;
        else if (filter == 4) {
            int c = prev && i >= bpp ? prev[i - bpp] : 0;
            int lo = a < b ? (int)a : (int)b;
            int hi = a < b ? (int)b : (int)a;
            int threshold = 3 * c - (int)a - (int)b;
            predictor = (unsigned)(threshold >= hi ? lo :
                                   threshold <= lo ? hi : c);
        }
        dst[i] = (uint8_t)(src[i] - predictor);
    }
}

PTPNG_API_INLINE __m256i encode_paeth16_avx2(__m256i a, __m256i b,
                                           __m256i c)
{
    __m256i lo = _mm256_min_epi16(a, b);
    __m256i hi = _mm256_max_epi16(a, b);
    __m256i threshold = _mm256_sub_epi16(
        _mm256_add_epi16(c, _mm256_slli_epi16(c, 1)),
        _mm256_add_epi16(a, b));
    /* Equality selects an edge, preserving Paeth's tie rules. */
    __m256i predictor = _mm256_blendv_epi8(
        hi, c, _mm256_cmpgt_epi16(threshold, lo));
    return _mm256_blendv_epi8(
        lo, predictor, _mm256_cmpgt_epi16(hi, threshold));
}

void ptpng_encode_filter_avx2(uint8_t *dst, const uint8_t *src,
                              const uint8_t *prev, size_t count,
                              unsigned bpp, int filter)
{
    size_t i = 0;
    const __m256i zero = _mm256_setzero_si256();
    if (!prev) {
        if (filter == 2) filter = 0;
        else if (filter == 4) filter = 1;
    }
    if (filter == 0) {
        if (count) memcpy(dst, src, count);
        return;
    }
    if (filter != 2) {
        i = count < bpp ? count : bpp;
        encode_tail_avx2(dst, src, prev, 0, i, bpp, filter);
    }
    switch (filter) {
    case 1:
        for (; count - i >= 32; i += 32) {
            __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
            __m256i a = _mm256_loadu_si256((const __m256i *)(src + i - bpp));
            _mm256_storeu_si256((__m256i *)(dst + i), _mm256_sub_epi8(s, a));
        }
        break;
    case 2:
        for (; count - i >= 32; i += 32) {
            __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
            __m256i b = _mm256_loadu_si256((const __m256i *)(prev + i));
            _mm256_storeu_si256((__m256i *)(dst + i), _mm256_sub_epi8(s, b));
        }
        break;
    case 3:
        for (; count - i >= 32; i += 32) {
            __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
            __m256i a = _mm256_loadu_si256((const __m256i *)(src + i - bpp));
            __m256i b = prev ? _mm256_loadu_si256((const __m256i *)(prev + i)) : zero;
            __m256i odd = _mm256_and_si256(_mm256_xor_si256(a, b), _mm256_set1_epi8(1));
            __m256i predictor = _mm256_sub_epi8(_mm256_avg_epu8(a, b), odd);
            _mm256_storeu_si256((__m256i *)(dst + i), _mm256_sub_epi8(s, predictor));
        }
        break;
    case 4:
        for (; count - i >= 32; i += 32) {
            __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
            __m256i a = _mm256_loadu_si256((const __m256i *)(src + i - bpp));
            __m256i b = _mm256_loadu_si256((const __m256i *)(prev + i));
            __m256i c = _mm256_loadu_si256((const __m256i *)(prev + i - bpp));
            __m256i lo = encode_paeth16_avx2(
                _mm256_unpacklo_epi8(a, zero), _mm256_unpacklo_epi8(b, zero),
                _mm256_unpacklo_epi8(c, zero));
            __m256i hi = encode_paeth16_avx2(
                _mm256_unpackhi_epi8(a, zero), _mm256_unpackhi_epi8(b, zero),
                _mm256_unpackhi_epi8(c, zero));
            __m256i predictor = _mm256_packus_epi16(lo, hi);
            _mm256_storeu_si256((__m256i *)(dst + i), _mm256_sub_epi8(s, predictor));
        }
        break;
    }
    encode_tail_avx2(dst, src, prev, i, count, bpp, filter);
}
