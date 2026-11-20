/* Forward PNG filters for ARM NEON. */
#include "ptpng_internal.h"

#if PTPNG_ARM_NEON

uint64_t ptpng_encode_score_neon(const uint8_t *src, size_t count)
{
    uint64_t score = 0;
    size_t i = 0;
    /* Accumulate 64 bytes per step in 16-bit lanes via vpadalq_u8. At most
     * 128 vectors per accumulator (4096 bytes) keeps each lane <= 32768. */
    while (count - i >= 64) {
        size_t chunk = count - i;
        size_t end;
        uint16x8_t a0 = vdupq_n_u16(0), a1 = vdupq_n_u16(0);
        if (chunk > 4096) chunk = 4096;
        end = i + (chunk & ~(size_t)63);
        for (; i < end; i += 64) {
            uint8x16_t m0 = vreinterpretq_u8_s8(vabsq_s8(
                vreinterpretq_s8_u8(vld1q_u8(src + i))));
            uint8x16_t m1 = vreinterpretq_u8_s8(vabsq_s8(
                vreinterpretq_s8_u8(vld1q_u8(src + i + 16))));
            uint8x16_t m2 = vreinterpretq_u8_s8(vabsq_s8(
                vreinterpretq_s8_u8(vld1q_u8(src + i + 32))));
            uint8x16_t m3 = vreinterpretq_u8_s8(vabsq_s8(
                vreinterpretq_s8_u8(vld1q_u8(src + i + 48))));
            a0 = vpadalq_u8(a0, m0);
            a1 = vpadalq_u8(a1, m1);
            a0 = vpadalq_u8(a0, m2);
            a1 = vpadalq_u8(a1, m3);
        }
        score += vaddvq_u32(vaddq_u32(vpaddlq_u16(a0), vpaddlq_u16(a1)));
    }
    for (; count - i >= 16; i += 16) {
        uint8x16_t mag = vreinterpretq_u8_s8(vabsq_s8(
            vreinterpretq_s8_u8(vld1q_u8(src + i))));
        score += vaddvq_u16(vpaddlq_u8(mag));
    }
    for (; i < count; ++i) {
        unsigned x = src[i];
        score += x < 128 ? x : 256-x;
    }
    return score;
}

static void encode_tail_neon(uint8_t *dst, const uint8_t *src,
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

void ptpng_encode_filter_neon(uint8_t *dst, const uint8_t *src,
                              const uint8_t *prev, size_t count,
                              unsigned bpp, int filter)
{
    size_t i = 0;
    const uint8x16_t zero = vdupq_n_u8(0);
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
        encode_tail_neon(dst, src, prev, 0, i, bpp, filter);
    }
    switch (filter) {
    case 1:
        for (; count - i >= 64; i += 64) {
            vst1q_u8(dst + i,      vsubq_u8(vld1q_u8(src + i),      vld1q_u8(src + i - bpp)));
            vst1q_u8(dst + i + 16, vsubq_u8(vld1q_u8(src + i + 16), vld1q_u8(src + i + 16 - bpp)));
            vst1q_u8(dst + i + 32, vsubq_u8(vld1q_u8(src + i + 32), vld1q_u8(src + i + 32 - bpp)));
            vst1q_u8(dst + i + 48, vsubq_u8(vld1q_u8(src + i + 48), vld1q_u8(src + i + 48 - bpp)));
        }
        for (; count - i >= 16; i += 16) {
            uint8x16_t s = vld1q_u8(src + i);
            uint8x16_t a = vld1q_u8(src + i - bpp);
            vst1q_u8(dst + i, vsubq_u8(s, a));
        }
        break;
    case 2:
        for (; count - i >= 64; i += 64) {
            vst1q_u8(dst + i,      vsubq_u8(vld1q_u8(src + i),      vld1q_u8(prev + i)));
            vst1q_u8(dst + i + 16, vsubq_u8(vld1q_u8(src + i + 16), vld1q_u8(prev + i + 16)));
            vst1q_u8(dst + i + 32, vsubq_u8(vld1q_u8(src + i + 32), vld1q_u8(prev + i + 32)));
            vst1q_u8(dst + i + 48, vsubq_u8(vld1q_u8(src + i + 48), vld1q_u8(prev + i + 48)));
        }
        for (; count - i >= 16; i += 16) {
            uint8x16_t s = vld1q_u8(src + i);
            uint8x16_t b = vld1q_u8(prev + i);
            vst1q_u8(dst + i, vsubq_u8(s, b));
        }
        break;
    case 3:
        if (prev) {
            for (; count - i >= 32; i += 32) {
                vst1q_u8(dst + i, vsubq_u8(vld1q_u8(src + i),
                    vhaddq_u8(vld1q_u8(src + i - bpp), vld1q_u8(prev + i))));
                vst1q_u8(dst + i + 16, vsubq_u8(vld1q_u8(src + i + 16),
                    vhaddq_u8(vld1q_u8(src + i + 16 - bpp), vld1q_u8(prev + i + 16))));
            }
        }
        for (; count - i >= 16; i += 16) {
            uint8x16_t s = vld1q_u8(src + i);
            uint8x16_t a = vld1q_u8(src + i - bpp);
            uint8x16_t b = prev ? vld1q_u8(prev + i) : zero;
            vst1q_u8(dst + i, vsubq_u8(s, vhaddq_u8(a, b)));
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
            vst1q_u8(dst + i, vsubq_u8(s, predictor));
        }
        break;
    }
    encode_tail_neon(dst, src, prev, i, count, bpp, filter);
}

#endif
