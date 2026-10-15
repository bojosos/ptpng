/* Forward PNG filters for ARM NEON. */
#include "ptpng_internal.h"

#if PTPNG_ARM_NEON

uint64_t ptpng_encode_score_neon(const uint8_t *src, size_t count)
{
    uint64x2_t sum = vdupq_n_u64(0);
    uint64_t score;
    size_t i = 0;
    for (; count - i >= 16; i += 16) {
        uint8x16_t magnitude = vreinterpretq_u8_s8(vabsq_s8(
            vreinterpretq_s8_u8(vld1q_u8(src + i))));
        /* Widen all the way to 64 bits: very wide rows can overflow 32 bits. */
        sum = vaddq_u64(sum, vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(magnitude))));
    }
    score = vgetq_lane_u64(sum, 0) + vgetq_lane_u64(sum, 1);
    for (; i < count; ++i) {
        unsigned x = src[i];
        score += x < 128 ? x : 256-x;
    }
    return score;
}

PTPNG_API_INLINE uint64_t encode_tail_neon(uint8_t *dst, const uint8_t *src,
                             const uint8_t *prev, size_t begin, size_t end,
                             unsigned bpp, int filter, int scoring)
{
    uint64_t score = 0;
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
        if (scoring) {
            unsigned x = dst[i];
            score += x < 128 ? x : 256-x;
        }
    }
    return score;
}

PTPNG_API_INLINE uint16x8_t encode_paeth8_neon(uint16x8_t au, uint16x8_t bu,
                                             uint16x8_t cu)
{
    int16x8_t a = vreinterpretq_s16_u16(au);
    int16x8_t b = vreinterpretq_s16_u16(bu);
    int16x8_t c = vreinterpretq_s16_u16(cu);
    int16x8_t lo = vminq_s16(a, b), hi = vmaxq_s16(a, b);
    int16x8_t threshold = vsubq_s16(vaddq_s16(c, vshlq_n_s16(c, 1)),
                                   vaddq_s16(a, b));
    int16x8_t predictor = vbslq_s16(vcgtq_s16(threshold, lo), c, hi);
    return vreinterpretq_u16_s16(vbslq_s16(vcgtq_s16(hi, threshold), predictor, lo));
}

PTPNG_API_INLINE uint64_t encode_filter_neon(uint8_t *dst, const uint8_t *src,
                              const uint8_t *prev, size_t count,
                              unsigned bpp, int filter, int scoring)
{
    size_t i = 0;
    uint64_t score = 0;
    uint64x2_t sum = vdupq_n_u64(0);
    const uint8x16_t zero = vdupq_n_u8(0);
    if (!prev) {
        if (filter == 2) filter = 0;
        else if (filter == 4) filter = 1;
    }
    if (filter == 0) {
        if (count) memcpy(dst, src, count);
        return scoring ? ptpng_encode_score_neon(src, count) : 0;
    }
    if (filter != 2) {
        i = count < bpp ? count : bpp;
        score = encode_tail_neon(dst, src, prev, 0, i, bpp, filter, scoring);
    }
    switch (filter) {
    case 1:
        for (; count - i >= 16; i += 16) {
            uint8x16_t s = vld1q_u8(src + i);
            uint8x16_t a = vld1q_u8(src + i - bpp);
            uint8x16_t residual = vsubq_u8(s, a);
            vst1q_u8(dst + i, residual);
            if (scoring) sum = vaddq_u64(sum, vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(
                vreinterpretq_u8_s8(vabsq_s8(vreinterpretq_s8_u8(residual)))))));
        }
        break;
    case 2:
        for (; count - i >= 16; i += 16) {
            uint8x16_t s = vld1q_u8(src + i);
            uint8x16_t b = vld1q_u8(prev + i);
            uint8x16_t residual = vsubq_u8(s, b);
            vst1q_u8(dst + i, residual);
            if (scoring) sum = vaddq_u64(sum, vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(
                vreinterpretq_u8_s8(vabsq_s8(vreinterpretq_s8_u8(residual)))))));
        }
        break;
    case 3:
        for (; count - i >= 16; i += 16) {
            uint8x16_t s = vld1q_u8(src + i);
            uint8x16_t a = vld1q_u8(src + i - bpp);
            uint8x16_t b = prev ? vld1q_u8(prev + i) : zero;
            uint8x16_t residual = vsubq_u8(s, vhaddq_u8(a, b));
            vst1q_u8(dst + i, residual);
            if (scoring) sum = vaddq_u64(sum, vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(
                vreinterpretq_u8_s8(vabsq_s8(vreinterpretq_s8_u8(residual)))))));
        }
        break;
    case 4:
        for (; count - i >= 16; i += 16) {
            uint8x16_t s = vld1q_u8(src + i);
            uint8x16_t a = vld1q_u8(src + i - bpp);
            uint8x16_t b = vld1q_u8(prev + i);
            uint8x16_t c = vld1q_u8(prev + i - bpp);
            uint16x8_t lo = encode_paeth8_neon(vmovl_u8(vget_low_u8(a)),
                vmovl_u8(vget_low_u8(b)), vmovl_u8(vget_low_u8(c)));
            uint16x8_t hi = encode_paeth8_neon(vmovl_u8(vget_high_u8(a)),
                vmovl_u8(vget_high_u8(b)), vmovl_u8(vget_high_u8(c)));
            uint8x16_t predictor = vcombine_u8(vmovn_u16(lo), vmovn_u16(hi));
            uint8x16_t residual = vsubq_u8(s, predictor);
            vst1q_u8(dst + i, residual);
            if (scoring) sum = vaddq_u64(sum, vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(
                vreinterpretq_u8_s8(vabsq_s8(vreinterpretq_s8_u8(residual)))))));
        }
        break;
    }
    score += encode_tail_neon(dst, src, prev, i, count, bpp, filter, scoring);
    if (scoring) score += vgetq_lane_u64(sum, 0) + vgetq_lane_u64(sum, 1);
    return score;
}

void ptpng_encode_filter_neon(uint8_t *dst, const uint8_t *src,
                              const uint8_t *prev, size_t count,
                              unsigned bpp, int filter)
{
    (void)encode_filter_neon(dst, src, prev, count, bpp, filter, 0);
}

uint64_t ptpng_encode_filter_score_neon(uint8_t *dst, const uint8_t *src,
                                       const uint8_t *prev, size_t count,
                                       unsigned bpp, int filter)
{
    return encode_filter_neon(dst, src, prev, count, bpp, filter, 1);
}

#endif
