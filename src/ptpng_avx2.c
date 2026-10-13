/*
 * ptpng_avx2.c - AVX2 accelerated paths.  Built with /arch:AVX2.
 * Runtime-dispatched from ptpng_cpu_init() via ptpng_simd_hook().
 */
#include "ptpng_internal.h"

/* ---- filters ---- */

PTPNG_API_INLINE __m256i bcast_byte_avx2(__m256i v)
{
    __m128i hi = _mm256_extracti128_si256(v, 1);
    return _mm256_broadcastsi128_si256(_mm_shuffle_epi8(hi, _mm_set1_epi8(15)));
}

/* Each block receives the preceding bpp reconstructed bytes, repeated
 * from lane zero. This also handles RGB pixels crossing register ends. */
#define SUB_PREFIX(v, BPP)                                               \
    (v) = _mm_add_epi8((v), _mm_slli_si128((v), (BPP)));                   \
    (v) = _mm_add_epi8((v), _mm_slli_si128((v), (BPP) * 2));               \
    (v) = _mm_add_epi8((v), _mm_slli_si128((v), (BPP) * 4));

#define SUB_BLOCKS(BPP, ...)                                             \
    {                                                                    \
        const __m128i ctrl = _mm_setr_epi8(__VA_ARGS__);                   \
        __m128i carry = _mm_setzero_si128();                              \
        size_t i = 0;                                                    \
        for (; i + 64 <= count; i += 64) {                               \
            __m128i v0 = _mm_loadu_si128((const __m128i *)(src + i));     \
            __m128i v1 = _mm_loadu_si128((const __m128i *)(src + i + 16));\
            __m128i v2 = _mm_loadu_si128((const __m128i *)(src + i + 32));\
            __m128i v3 = _mm_loadu_si128((const __m128i *)(src + i + 48));\
            SUB_PREFIX(v0, BPP) SUB_PREFIX(v1, BPP)                      \
            SUB_PREFIX(v2, BPP) SUB_PREFIX(v3, BPP)                      \
            v0 = _mm_add_epi8(v0, carry);                                \
            v1 = _mm_add_epi8(v1, _mm_shuffle_epi8(v0, ctrl));           \
            v2 = _mm_add_epi8(v2, _mm_shuffle_epi8(v1, ctrl));           \
            v3 = _mm_add_epi8(v3, _mm_shuffle_epi8(v2, ctrl));           \
            carry = _mm_shuffle_epi8(v3, ctrl);                          \
            _mm_storeu_si128((__m128i *)(dst + i), v0);                  \
            _mm_storeu_si128((__m128i *)(dst + i + 16), v1);             \
            _mm_storeu_si128((__m128i *)(dst + i + 32), v2);             \
            _mm_storeu_si128((__m128i *)(dst + i + 48), v3);             \
        }                                                                \
        for (; i + 16 <= count; i += 16) {                               \
            __m128i v = _mm_loadu_si128((const __m128i *)(src + i));      \
            SUB_PREFIX(v, BPP)                                          \
            v = _mm_add_epi8(v, carry);                                  \
            carry = _mm_shuffle_epi8(v, ctrl);                           \
            _mm_storeu_si128((__m128i *)(dst + i), v);                   \
        }                                                                \
        for (; i < count; i++)                                          \
            dst[i] = (uint8_t)(src[i] + (i >= (BPP) ? dst[i - (BPP)] : 0));\
        return;                                                          \
    }

void ptpng_filter_sub_avx2(uint8_t *dst, const uint8_t *src,
                           const uint8_t *prev, size_t count, unsigned bpp)
{
    uint8_t *row = dst;
    (void)prev;
    /* Keep the established SSE2 kernel for long, register-aligned pixels.
     * Fusing the copy regresses these strides on some x86 processors. */
    if (count >= 256 && (bpp == 2 || bpp == 4 || bpp == 8)) {
        memmove(dst, src, count);
        ptpng_sub_blocked_sse2(dst, count, bpp);
        return;
    }
    if (bpp == 1) {
        /* note: _mm256_slli_si256 shifts per 128-bit lane, so the
         * in-register doubling yields two independent 16-byte prefixes;
         * a lane-total addend fixes the high lane afterwards. */
        const __m256i mask_hi = _mm256_setr_epi32(0, 0, 0, 0, -1, -1, -1, -1);
        size_t i = 0;
        uint8_t carry = 0;
        for (; i + 128 <= count; i += 128) {
            __m256i v0 = _mm256_loadu_si256((const __m256i *)(src + i));
            __m256i v1 = _mm256_loadu_si256((const __m256i *)(src + i + 32));
            __m256i v2 = _mm256_loadu_si256((const __m256i *)(src + i + 64));
            __m256i v3 = _mm256_loadu_si256((const __m256i *)(src + i + 96));
#define PREFIX16_256(v)                                                  \
            (v) = _mm256_add_epi8((v), _mm256_slli_si256((v), 1));       \
            (v) = _mm256_add_epi8((v), _mm256_slli_si256((v), 2));       \
            (v) = _mm256_add_epi8((v), _mm256_slli_si256((v), 4));       \
            (v) = _mm256_add_epi8((v), _mm256_slli_si256((v), 8));
            PREFIX16_256(v0)
            PREFIX16_256(v1)
            PREFIX16_256(v2)
            PREFIX16_256(v3)
#undef PREFIX16_256
            {
                __m256i t0 = _mm256_set1_epi8((char)_mm256_extract_epi8(v0, 15));
                __m256i t1 = _mm256_set1_epi8((char)_mm256_extract_epi8(v1, 15));
                __m256i t2 = _mm256_set1_epi8((char)_mm256_extract_epi8(v2, 15));
                __m256i t3 = _mm256_set1_epi8((char)_mm256_extract_epi8(v3, 15));
                v0 = _mm256_add_epi8(v0, _mm256_and_si256(t0, mask_hi));
                v1 = _mm256_add_epi8(v1, _mm256_and_si256(t1, mask_hi));
                v2 = _mm256_add_epi8(v2, _mm256_and_si256(t2, mask_hi));
                v3 = _mm256_add_epi8(v3, _mm256_and_si256(t3, mask_hi));
            }
            {
                __m256i c = _mm256_set1_epi8((char)carry);
                v0 = _mm256_add_epi8(v0, c);
                v1 = _mm256_add_epi8(v1, bcast_byte_avx2(v0));
                v2 = _mm256_add_epi8(v2, bcast_byte_avx2(v1));
                v3 = _mm256_add_epi8(v3, bcast_byte_avx2(v2));
                carry = (uint8_t)_mm256_extract_epi8(v3, 31);
            }
            _mm256_storeu_si256((__m256i *)(row + i), v0);
            _mm256_storeu_si256((__m256i *)(row + i + 32), v1);
            _mm256_storeu_si256((__m256i *)(row + i + 64), v2);
            _mm256_storeu_si256((__m256i *)(row + i + 96), v3);
        }
        for (; i + 16 <= count; i += 16) {
            __m128i v = _mm_loadu_si128((const __m128i *)(src + i));
            v = _mm_add_epi8(v, _mm_slli_si128(v, 1));
            v = _mm_add_epi8(v, _mm_slli_si128(v, 2));
            v = _mm_add_epi8(v, _mm_slli_si128(v, 4));
            v = _mm_add_epi8(v, _mm_slli_si128(v, 8));
            v = _mm_add_epi8(v, _mm_set1_epi8((char)carry));
            carry = (uint8_t)_mm_extract_epi8(v, 15);
            _mm_storeu_si128((__m128i *)(row + i), v);
        }
        for (; i < count; i++) {
            row[i] = (uint8_t)(src[i] + carry);
            carry = row[i];
        }
        return;
    }

    switch (bpp) {
    case 2: SUB_BLOCKS(2, 14,15,14,15,14,15,14,15,14,15,14,15,14,15,14,15)
    case 3: SUB_BLOCKS(3, 13,14,15,13,14,15,13,14,15,13,14,15,13,14,15,13)
    case 4: SUB_BLOCKS(4, 12,13,14,15,12,13,14,15,12,13,14,15,12,13,14,15)
    case 6: SUB_BLOCKS(6, 10,11,12,13,14,15,10,11,12,13,14,15,10,11,12,13)
    case 8: SUB_BLOCKS(8, 8,9,10,11,12,13,14,15,8,9,10,11,12,13,14,15)
    default: ptpng_filter_sub_scalar(dst, src, prev, count, bpp);
    }
}

#undef SUB_BLOCKS
#undef SUB_PREFIX

void ptpng_filter_up_avx2(uint8_t *dst, const uint8_t *src,
                          const uint8_t *prev, size_t count, unsigned bpp)
{
    size_t i = 0;
    (void)bpp;
    for (; i + 128 <= count; i += 128) {
        __m256i a0 = _mm256_loadu_si256((const __m256i *)(src + i));
        __m256i a1 = _mm256_loadu_si256((const __m256i *)(src + i + 32));
        __m256i a2 = _mm256_loadu_si256((const __m256i *)(src + i + 64));
        __m256i a3 = _mm256_loadu_si256((const __m256i *)(src + i + 96));
        __m256i b0 = _mm256_loadu_si256((const __m256i *)(prev + i));
        __m256i b1 = _mm256_loadu_si256((const __m256i *)(prev + i + 32));
        __m256i b2 = _mm256_loadu_si256((const __m256i *)(prev + i + 64));
        __m256i b3 = _mm256_loadu_si256((const __m256i *)(prev + i + 96));
        _mm256_storeu_si256((__m256i *)(dst + i),      _mm256_add_epi8(a0, b0));
        _mm256_storeu_si256((__m256i *)(dst + i + 32), _mm256_add_epi8(a1, b1));
        _mm256_storeu_si256((__m256i *)(dst + i + 64), _mm256_add_epi8(a2, b2));
        _mm256_storeu_si256((__m256i *)(dst + i + 96), _mm256_add_epi8(a3, b3));
    }
    for (; i + 32 <= count; i += 32) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(src + i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(prev + i));
        _mm256_storeu_si256((__m256i *)(dst + i), _mm256_add_epi8(a, b));
    }
    for (; i < count; i++)
        dst[i] = (uint8_t)(src[i] + prev[i]);
}

/* The bytes of a four- or eight-byte pixel are independent Paeth chains. Keep
 * the decoded pixel in 16-bit lanes and select all predictors together.
 * Scalar remains faster for three-byte pixels on some x86 cores. */
void ptpng_filter_paeth_avx2(uint8_t *dst, const uint8_t *src,
                             const uint8_t *prev, size_t count, unsigned bpp)
{
    size_t i = 0;
    __m128i a = _mm_setzero_si128(), c = _mm_setzero_si128();
    const __m128i mask = _mm_set1_epi16(255);
    if (bpp != 4) {
        if (bpp == 8) {
            for (; i + 8 <= count; i += 8) {
                __m128i b, s, lo, hi, threshold, predictor;
                b = _mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *)(prev + i)));
                s = _mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *)(src + i)));
                lo = _mm_min_epi16(a, b);
                hi = _mm_max_epi16(a, b);
                threshold = _mm_sub_epi16(_mm_add_epi16(c, _mm_slli_epi16(c, 1)),
                                          _mm_add_epi16(a, b));
                predictor = _mm_blendv_epi8(hi, c, _mm_cmpgt_epi16(threshold, lo));
                predictor = _mm_blendv_epi8(lo, predictor, _mm_cmpgt_epi16(hi, threshold));
                a = _mm_and_si128(_mm_add_epi16(s, predictor), mask);
                c = b;
                _mm_storel_epi64((__m128i *)(dst + i), _mm_packus_epi16(a, a));
            }
            goto scalar_tail;
        }
        ptpng_filter_paeth_scalar(dst, src, prev, count, bpp);
        return;
    }
    for (; i + 4 <= count; i += 4) {
        uint32_t sv, bv, out;
        __m128i b, s, lo, hi, threshold, predictor;
        memcpy(&sv, src + i, 4);
        memcpy(&bv, prev + i, 4);
        b = _mm_cvtepu8_epi16(_mm_cvtsi32_si128((int)bv));
        s = _mm_cvtepu8_epi16(_mm_cvtsi32_si128((int)sv));
        lo = _mm_min_epi16(a, b);
        hi = _mm_max_epi16(a, b);
        /* Values span -510..765, safely inside signed 16-bit lanes.
         * Threshold equality must select a or b, never c. */
        threshold = _mm_sub_epi16(_mm_add_epi16(c, _mm_slli_epi16(c, 1)),
                                  _mm_add_epi16(a, b));
        predictor = _mm_blendv_epi8(hi, c, _mm_cmpgt_epi16(threshold, lo));
        predictor = _mm_blendv_epi8(lo, predictor, _mm_cmpgt_epi16(hi, threshold));
        a = _mm_and_si128(_mm_add_epi16(s, predictor), mask);
        c = b;
        out = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(a, a));
        memcpy(dst + i, &out, 4);
    }
scalar_tail:
    for (; i < count; i++) {
        int a = i >= bpp ? dst[i - bpp] : 0;
        int b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
        int lo = a < b ? a : b, hi = a < b ? b : a;
        int threshold = 3 * c - a - b;
        int predictor = threshold >= hi ? lo : threshold <= lo ? hi : c;
        dst[i] = (uint8_t)(src[i] + predictor);
    }
}

/* ---- conversion tables (filled from scalar, hot entries overridden) ---- */

ptpng_cvt_fn ptpng_cvt_table_rgba8_avx2[128];
ptpng_cvt_fn ptpng_cvt_table_rgb8_avx2[128];

/* gray8 -> RGBA8: duplicate each byte to g,g,g and force alpha=255.
 * gg holds each value twice, so controls index 0,2,4,6. */
static void rgba8_g8_avx2(const uint8_t *src, uint8_t *dst, uint32_t n,
                          const struct ptpng_cvt *c)
{
    const __m128i dup_lo = _mm_setr_epi8(0, 0, 0, -1, 2, 2, 2, -1,
                                         4, 4, 4, -1, 6, 6, 6, -1);
    const __m128i dup_hi = _mm_setr_epi8(8, 8, 8, -1, 10, 10, 10, -1,
                                         12, 12, 12, -1, 14, 14, 14, -1);
    const __m128i alpha = _mm_setr_epi8(0, 0, 0, -1, 0, 0, 0, -1,
                                        0, 0, 0, -1, 0, 0, 0, -1);
    uint32_t i = 0;
    (void)c;
    for (; i + 8 <= n; i += 8) {
        __m128i g = _mm_loadl_epi64((const __m128i *)(src + i));
        __m128i gg = _mm_unpacklo_epi8(g, g);   /* g0 g0 g1 g1 ... g7 g7 */
        _mm_storeu_si128((__m128i *)(dst + i * 4),
                         _mm_or_si128(_mm_shuffle_epi8(gg, dup_lo), alpha));
        _mm_storeu_si128((__m128i *)(dst + i * 4 + 16),
                         _mm_or_si128(_mm_shuffle_epi8(gg, dup_hi), alpha));
    }
    for (; i < n; i++) {
        uint8_t gv = src[i];
        dst[i * 4] = dst[i * 4 + 1] = dst[i * 4 + 2] = gv;
        dst[i * 4 + 3] = 255;
    }
}

/* gray+alpha 8-bit -> RGBA8: [g,g,g,a] */
static void rgba8_ga8_avx2(const uint8_t *src, uint8_t *dst, uint32_t n,
                           const struct ptpng_cvt *c)
{
    const __m128i lo = _mm_setr_epi8(0, 0, 0, 1, 2, 2, 2, 3,
                                     4, 4, 4, 5, 6, 6, 6, 7);
    const __m128i hi = _mm_setr_epi8(8, 8, 8, 9, 10, 10, 10, 11,
                                     12, 12, 12, 13, 14, 14, 14, 15);
    uint32_t i = 0;
    (void)c;
    for (; i + 8 <= n; i += 8) {
        __m128i v = _mm_loadu_si128((const __m128i *)(src + i * 2));
        _mm_storeu_si128((__m128i *)(dst + i * 4),
                         _mm_shuffle_epi8(v, lo));
        _mm_storeu_si128((__m128i *)(dst + i * 4 + 16),
                         _mm_shuffle_epi8(v, hi));
    }
    for (; i < n; i++) {
        uint8_t g = src[i * 2];
        dst[i * 4] = dst[i * 4 + 1] = dst[i * 4 + 2] = g;
        dst[i * 4 + 3] = src[i * 2 + 1];
    }
}

/* rgba 16-bit BE -> RGBA8: keep the high byte of each sample.  Each
 * 32-byte lane carries 4 pixels; pshufb picks bytes 0,2,4,6,8,10,12,14
 * lane-locally, then an unpack compacts the two lane halves. */
static void rgba8_rgba16_avx2(const uint8_t *src, uint8_t *dst, uint32_t n,
                              const struct ptpng_cvt *c)
{
    const __m256i pick = _mm256_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14,
                                          -1, -1, -1, -1, -1, -1, -1, -1,
                                          0, 2, 4, 6, 8, 10, 12, 14,
                                          -1, -1, -1, -1, -1, -1, -1, -1);
    uint32_t i = 0;
    (void)c;
    for (; i + 4 <= n; i += 4) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(src + i * 8));
        __m256i sh = _mm256_shuffle_epi8(v, pick);
        __m128i lo = _mm256_castsi256_si128(sh);        /* px0 px1 */
        __m128i hi = _mm256_extracti128_si256(sh, 1);    /* px2 px3 */
        _mm_storeu_si128((__m128i *)(dst + i * 4),
                         _mm_unpacklo_epi64(lo, hi));
    }
    for (; i < n; i++) {
        dst[i * 4] = src[i * 8];
        dst[i * 4 + 1] = src[i * 8 + 2];
        dst[i * 4 + 2] = src[i * 8 + 4];
        dst[i * 4 + 3] = src[i * 8 + 6];
    }
}

/* gray 16-bit BE -> RGBA8 */
static void rgba8_g16_avx2(const uint8_t *src, uint8_t *dst, uint32_t n,
                           const struct ptpng_cvt *c)
{
    const __m128i pick = _mm_setr_epi8(0, 0, 0, -1, 2, 2, 2, -1,
                                       4, 4, 4, -1, 6, 6, 6, -1);
    const __m128i pick2 = _mm_setr_epi8(8, 8, 8, -1, 10, 10, 10, -1,
                                        12, 12, 12, -1, 14, 14, 14, -1);
    const __m128i alpha = _mm_setr_epi8(0, 0, 0, -1, 0, 0, 0, -1,
                                        0, 0, 0, -1, 0, 0, 0, -1);
    uint32_t i = 0;
    (void)c;
    for (; i + 8 <= n; i += 8) {
        __m128i v = _mm_loadu_si128((const __m128i *)(src + i * 2));
        _mm_storeu_si128((__m128i *)(dst + i * 4),
                         _mm_or_si128(_mm_shuffle_epi8(v, pick), alpha));
        _mm_storeu_si128((__m128i *)(dst + i * 4 + 16),
                         _mm_or_si128(_mm_shuffle_epi8(v, pick2), alpha));
    }
    for (; i < n; i++) {
        uint8_t g = src[i * 2];
        dst[i * 4] = dst[i * 4 + 1] = dst[i * 4 + 2] = g;
        dst[i * 4 + 3] = 255;
    }
}

/* rgb 16-bit BE -> RGBA8 (2 pixels per 12 input bytes) */
static void rgba8_rgb16_avx2(const uint8_t *src, uint8_t *dst, uint32_t n,
                             const struct ptpng_cvt *c)
{
    const __m128i lo = _mm_setr_epi8(0, 2, 4, -1, 6, 8, 10, -1,
                                     0, 0, 0, 0, 0, 0, 0, 0);
    const __m128i alpha = _mm_setr_epi8(0, 0, 0, -1, 0, 0, 0, -1,
                                        0, 0, 0, 0, 0, 0, 0, 0);
    uint32_t i = 0;
    (void)c;
    for (; i + 3 <= n; i += 2) {
        __m128i v = _mm_loadu_si128((const __m128i *)(src + i * 6));
        __m128i out = _mm_or_si128(_mm_shuffle_epi8(v, lo), alpha);
        _mm_storel_epi64((__m128i *)(dst + i * 4), out);
    }
    for (; i < n; i++) {
        dst[i * 4] = src[i * 6];
        dst[i * 4 + 1] = src[i * 6 + 2];
        dst[i * 4 + 2] = src[i * 6 + 4];
        dst[i * 4 + 3] = 255;
    }
}

/* RGB8 -> RGBA8. Two overlapping, bounded loads supply eight pixels;
 * shifting the second load puts four RGB triples in each AVX2 lane. */
static void rgba8_rgb8_avx2(const uint8_t *src, uint8_t *dst, uint32_t n,
                            const struct ptpng_cvt *c)
{
    const __m128i ctrl = _mm_setr_epi8(0, 1, 2, -1, 3, 4, 5, -1,
                                       6, 7, 8, -1, 9, 10, 11, -1);
    const __m128i alpha = _mm_setr_epi8(0, 0, 0, -1, 0, 0, 0, -1,
                                        0, 0, 0, -1, 0, 0, 0, -1);
    uint32_t i = 0;
#if PTPNG_REUSE_CVT_BUFFER
    if (c->reverse) {
        while (n >= 8) {
            const uint8_t *s;
            __m128i lo, hi;
            __m256i v;
            n -= 8; s = src + (size_t)n * 3;
            lo = _mm_loadu_si128((const __m128i *)s);
            hi = _mm_loadu_si128((const __m128i *)(s + 8));
            v = _mm256_inserti128_si256(_mm256_castsi128_si256(lo),
                                       _mm_srli_si128(hi, 4), 1);
            v = _mm256_shuffle_epi8(v, _mm256_broadcastsi128_si256(ctrl));
            _mm256_storeu_si256((__m256i *)(dst + (size_t)n * 4),
                               _mm256_or_si256(v, _mm256_broadcastsi128_si256(alpha)));
        }
        while (n) {
            uint8_t r, g, b;
            --n;
            r = src[(size_t)n * 3]; g = src[(size_t)n * 3 + 1];
            b = src[(size_t)n * 3 + 2];
            dst[(size_t)n * 4] = r; dst[(size_t)n * 4 + 1] = g;
            dst[(size_t)n * 4 + 2] = b; dst[(size_t)n * 4 + 3] = 255;
        }
        return;
    }
#else
    (void)c;
#endif
    for (; n - i >= 8; i += 8) {
        __m128i lo = _mm_loadu_si128((const __m128i *)(src + i * 3));
        __m128i hi = _mm_loadu_si128((const __m128i *)(src + i * 3 + 8));
        __m256i v = _mm256_inserti128_si256(_mm256_castsi128_si256(lo),
                                           _mm_srli_si128(hi, 4), 1);
        v = _mm256_shuffle_epi8(v, _mm256_broadcastsi128_si256(ctrl));
        _mm256_storeu_si256((__m256i *)(dst + i * 4),
                           _mm256_or_si256(v, _mm256_broadcastsi128_si256(alpha)));
    }
    for (; i + 6 <= n; i += 4) {
        __m128i v = _mm_loadu_si128((const __m128i *)(src + i * 3));
        _mm_storeu_si128((__m128i *)(dst + i * 4),
                         _mm_or_si128(_mm_shuffle_epi8(v, ctrl), alpha));
    }
    for (; i < n; i++) {
        dst[i * 4] = src[i * 3];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 255;
    }
}

/* RGBA8 -> RGB8. Compact each lane to twelve bytes, then join the lanes
 * with exact sixteen- and eight-byte stores. Alpha is discarded. */
static void rgb8_rgba8_avx2(const uint8_t *src, uint8_t *dst, uint32_t n,
                            const struct ptpng_cvt *c)
{
    const __m128i ctrl = _mm_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9,
                                       10, 12, 13, 14, -1, -1, -1, -1);
    (void)c;
    for (; n >= 8; n -= 8, src += 32, dst += 24) {
        __m256i v = _mm256_loadu_si256((const __m256i *)src);
        __m128i lo, hi;
        v = _mm256_shuffle_epi8(v, _mm256_broadcastsi128_si256(ctrl));
        lo = _mm256_castsi256_si128(v);
        hi = _mm256_extracti128_si256(v, 1);
        _mm_storeu_si128((__m128i *)dst,
                        _mm_or_si128(lo, _mm_slli_si128(hi, 12)));
        _mm_storel_epi64((__m128i *)(dst + 16), _mm_srli_si128(hi, 4));
    }
    if (n >= 4) {
        __m128i v = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)src), ctrl);
        uint32_t tail = (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(v, 8));
        _mm_storel_epi64((__m128i *)dst, v);
        memcpy(dst + 8, &tail, sizeof(tail));
        n -= 4; src += 16; dst += 12;
    }
    for (; n; --n, src += 4, dst += 3) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
    }
}

/* palette 8-bit -> RGBA8 via dword gather from the precombined table */
static void rgba8_p8_avx2(const uint8_t *src, uint8_t *dst, uint32_t n,
                          const struct ptpng_cvt *c)
{
    const uint32_t *tab = c->pal_words;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i idx = _mm256_cvtepu8_epi32(
            _mm_loadl_epi64((const __m128i *)(src + i)));
        __m256i v = _mm256_i32gather_epi32((const int *)tab, idx, 4);
        _mm256_storeu_si256((__m256i *)(dst + i * 4), v);
    }
    for (; i < n; i++) {
        const uint8_t *e = c->pal_rgba + src[i] * 4;
        dst[i * 4] = e[0];
        dst[i * 4 + 1] = e[1];
        dst[i * 4 + 2] = e[2];
        dst[i * 4 + 3] = e[3];
    }
}

#if defined(_MSC_VER) && !defined(__clang__)
/* MSVC Palette8 -> RGB8: gather eight packed colors, remove alpha and store
 * exactly twenty-four bytes. No source or output padding is required. */
static void rgb8_p8_avx2(const uint8_t *src, uint8_t *dst, uint32_t n,
                         const struct ptpng_cvt *c)
{
    const __m128i ctrl = _mm_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9,
                                       10, 12, 13, 14, -1, -1, -1, -1);
    for (; n >= 8; n -= 8, src += 8, dst += 24) {
        __m256i idx = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)src));
        __m256i v = _mm256_i32gather_epi32((const int *)c->pal_words, idx, 4);
        __m128i lo, hi;
        v = _mm256_shuffle_epi8(v, _mm256_broadcastsi128_si256(ctrl));
        lo = _mm256_castsi256_si128(v);
        hi = _mm256_extracti128_si256(v, 1);
        _mm_storeu_si128((__m128i *)dst,
                        _mm_or_si128(lo, _mm_slli_si128(hi, 12)));
        _mm_storel_epi64((__m128i *)(dst + 16), _mm_srli_si128(hi, 4));
    }
    for (; n; --n, ++src, dst += 3)
        memcpy(dst, c->pal_rgba + (size_t)*src * 4, 3);
}
#endif

static uint32_t adler32_avx2(const uint8_t *p, size_t n)
{
    const __m256i zero = _mm256_setzero_si256();
    const __m256i ones = _mm256_set1_epi16(1);
    const __m256i weights = _mm256_setr_epi8(
        32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,
        16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1);
    uint32_t a = 1, b = 0;

    while (n >= 32) {
        unsigned chunk = (unsigned)(n < 4096 ? n : 4096) & ~31u;
        __m256i sum = zero, prefix = zero, weighted = zero;
        uint64_t sums[4], prefixes[4];
        uint32_t terms[8], s = 0, w = 0;
        unsigned i;
        for (i = 0; i < chunk; i += 32) {
            __m256i bytes = _mm256_loadu_si256((const __m256i *)(p + i));
            prefix = _mm256_add_epi64(prefix, sum);
            sum = _mm256_add_epi64(sum, _mm256_sad_epu8(bytes, zero));
            /* Adjacent weighted bytes sum to at most 255*(32+31),
             * so the signed saturating multiply-add cannot saturate. */
            weighted = _mm256_add_epi32(weighted, _mm256_madd_epi16(
                _mm256_maddubs_epi16(bytes, weights), ones));
        }
        _mm256_storeu_si256((__m256i *)sums, sum);
        _mm256_storeu_si256((__m256i *)prefixes, prefix);
        _mm256_storeu_si256((__m256i *)terms, weighted);
        for (i = 0; i < 4; i++) {
            s += (uint32_t)sums[i];
            w += 32u * (uint32_t)prefixes[i];
        }
        for (i = 0; i < 8; i++) w += terms[i];
        /* 4096*65520 + 255*4096*4097/2 + 65520 < 2^32. */
        b = (b + chunk * a + w) % 65521u;
        a = (a + s) % 65521u;
        p += chunk;
        n -= chunk;
    }
    while (n--) {
        a += *p++;
        b += a;
    }
    return ((b % 65521u) << 16) | (a % 65521u);
}

void ptpng_avx2_init(void)
{
    memcpy(ptpng_cvt_table_rgba8_avx2, ptpng_cvt_table_rgba8_scalar,
           sizeof(ptpng_cvt_table_rgba8_avx2));
    memcpy(ptpng_cvt_table_rgb8_avx2, ptpng_cvt_table_rgb8_scalar,
           sizeof(ptpng_cvt_table_rgb8_avx2));

    if (ptpng_cpu.avx2) {
        ptpng_cpu.adler32 = adler32_avx2;
        ptpng_cpu.filter_sub = ptpng_filter_sub_avx2;
        ptpng_cpu.filter_up = ptpng_filter_up_avx2;
        ptpng_cpu.filter_paeth = ptpng_filter_paeth_avx2;
        ptpng_cpu.cvt_table_rgba8 = ptpng_cvt_table_rgba8_avx2;
        ptpng_cpu.cvt_table_rgb8 = ptpng_cvt_table_rgb8_avx2;

        ptpng_cvt_table_rgba8_avx2[(0 << 4) | 3] = rgba8_g8_avx2;
        ptpng_cvt_table_rgba8_avx2[(2 << 4) | 3] = rgba8_rgb8_avx2;
        ptpng_cvt_table_rgba8_avx2[(4 << 4) | 3] = rgba8_ga8_avx2;
        ptpng_cvt_table_rgba8_avx2[(0 << 4) | 4] = rgba8_g16_avx2;
        ptpng_cvt_table_rgba8_avx2[(2 << 4) | 4] = rgba8_rgb16_avx2;
        ptpng_cvt_table_rgba8_avx2[(6 << 4) | 4] = rgba8_rgba16_avx2;
        ptpng_cvt_table_rgba8_avx2[(3 << 4) | 3] = rgba8_p8_avx2;
        ptpng_cvt_table_rgb8_avx2[(6 << 4) | 3] = rgb8_rgba8_avx2;
#if defined(_MSC_VER) && !defined(__clang__)
        /* GCC's scalar packed-palette lookup is faster than RGB gathering. */
        ptpng_cvt_table_rgb8_avx2[(3 << 4) | 3] = rgb8_p8_avx2;
#endif
    }
}
